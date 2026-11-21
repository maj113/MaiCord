#pragma once

#include "maicord/ui/layout/layout_engine.hpp"
#include "maicord/ui/text/style.hpp"

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
    UiTextFace face;
};

struct UiTextMeasureValue {
    UiSize size;
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
};

bool ui_text_measure_cache_initialize(UiTextMeasureCache* cache, Slice<UiTextMeasureRecord> storage,
                                      UiTextMeasureBackend backend, void* backend_context) noexcept;
void ui_text_measure_cache_invalidate(UiTextMeasureCache* cache) noexcept;
UiTextMeasureResult ui_text_measure_cache_measure(UiTextMeasureCache* cache,
                                                  const UiTextMeasureRequest& request,
                                                  UiTextMeasureValue* value) noexcept;

// Which failed measurements the text system logs. Each source revision (a message revision) logs once, and at most
// kUiTextMeasureFailureLogEntries of them log until the owner empties the log after a run without failures. A text
// that never measures then writes one line, not one line for each retry.
inline constexpr u32 kUiTextMeasureFailureLogEntries = 16;
struct UiTextMeasureFailureLog {
    u64 sources[kUiTextMeasureFailureLogEntries];
    u32 revisions[kUiTextMeasureFailureLogEntries];
    u32 count;
};
// True for the first failure of a source revision since the log was emptied; it then records the revision.
bool ui_text_measure_failure_log_due(UiTextMeasureFailureLog* log, const UiTextMeasureKey& key) noexcept;

static_assert(sizeof(UiTextMeasureKey) == 24);
static_assert(sizeof(UiTextMeasureValue) == 12);
static_assert(sizeof(UiTextMeasureRecord) == 64);
static_assert(sizeof(UiTextMeasureCache) == 48);

} // namespace maicord

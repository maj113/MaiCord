#pragma once
#include "maicord/protocol/message_search.hpp"

namespace maicord {
enum class UiSearchQueryError : u8 {
    none,
    invalid_text,
    capacity,
    invalid_filter,
    invalid_date
};
// Filters accept snowflakes or native mention syntax; labels never confer identity.
struct UiSearchQueryResult {
    MessageSearchQuery query;
    UiSearchQueryError error;
    u32 error_offset;
    constexpr bool ok() const noexcept {
        return error == UiSearchQueryError::none;
    }
};
struct UiSearchQueryContext {
    void* context;
    bool (*user)(void*, Utf8View, u64*) noexcept;
    bool (*channel)(void*, Utf8View, u64*) noexcept;
    // Local calendar boundaries, expressed as milliseconds since the snowflake epoch.
    bool (*date)(void*, Utf8View, u64* start, u64* end) noexcept;
};
UiSearchQueryResult ui_search_query_parse(Utf8View text, MessageSearchScope scope, u64 scope_id,
                                          MessageSearchSort sort,
                                          const UiSearchQueryContext* context = nullptr) noexcept;
} // namespace maicord

#pragma once
#include "maicord/core/log.hpp"
#include "maicord/core/trace_record.hpp"

namespace maicord::app_detail {
// Logs `count` values, each under the next key of the NUL-separated `keys` list, at one level. The
// list carries one literal for the whole group instead of one call per value.
__declspec(noinline) inline void log_u64_fields_impl(LogLevel level, const char* component, const char* keys,
                                                     const u64* values, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        log_u64(level, component, keys, values[i]);
        while (*keys++) {
        }
    }
}
// `keys` is "first\0second\0..." (the literal's own terminator closes the last key).
template <u32 N>
__forceinline void log_u64_fields(LogLevel level, const char* component, const char* keys,
                                  const u64 (&values)[N]) noexcept {
    log_u64_fields_impl(level, component, keys, values, N);
}
// Two values; no array on the caller's stack.
__declspec(noinline) inline void log_u64_pair(LogLevel level, const char* component, const char* keys, u64 first,
                                              u64 second) noexcept {
    log_u64(level, component, keys, first);
    while (*keys++) {
    }
    log_u64(level, component, keys, second);
}
// Appends `count` numeric fields to a trace record, named by the NUL-separated `keys` list.
__declspec(noinline) inline void trace_fields_impl(TraceRecord& trace, const char* keys, const u64* values,
                                                   u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        trace.field(keys, values[i]);
        while (*keys++) {
        }
    }
}
template <u32 N>
__forceinline void trace_fields(TraceRecord& trace, const char* keys, const u64 (&values)[N]) noexcept {
    trace_fields_impl(trace, keys, values, N);
}
} // namespace maicord::app_detail

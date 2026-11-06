#pragma once
#include "maicord/core/log_event.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_process.hpp"
#else
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord {
// A LogEvent that carries the process id and writes at info level: the trace lines measurement tools match by
// process (load_trace stages, REST phases, startup marks). No allocations or output on polling paths unless a state
// actually changes.
struct TraceRecord : LogEvent {
    explicit TraceRecord(const char* event) noexcept : LogEvent(event) {
        field("pid", process_id());
    }
    using LogEvent::write;
    void write(const char* component) const noexcept {
        LogEvent::write(LogLevel::info, component);
    }
};
} // namespace maicord

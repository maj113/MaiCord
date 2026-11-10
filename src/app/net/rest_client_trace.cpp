#include "rest_client_internal.hpp"
#include "maicord/core/log.hpp"

namespace maicord::rest_client_detail {

void rest_trace(const char* record) noexcept {
    log_write(LogLevel::info, "rest_trace", record);
}

} // namespace maicord::rest_client_detail

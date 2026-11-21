// host_iana_timezone on Linux: the zone the TZif loader of include/maicord/os/local_time.hpp found ($TZ, /etc/localtime
// or /etc/timezone), by its IANA identifier.
#include "maicord/platform/desktop/timezone.hpp"
#include "maicord/os/local_time.hpp"

namespace maicord {
// local_zone_name keeps identifiers of at most 63 bytes, inside kIanaTimezoneMaximumBytes, and refuses one that does not
// fit `output` with an empty string, as this contract asks.
u32 host_iana_timezone(Slice<char> output) noexcept {
    return local_zone_name(output);
}
} // namespace maicord

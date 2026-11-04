// host_iana_timezone on Linux: the zone the TZif loader of include/maicord/os/local_time.hpp found ($TZ, /etc/localtime
// or /etc/timezone), by its IANA identifier.
#include "maicord/platform/desktop/timezone.hpp"
#include "maicord/os/local_time.hpp"

namespace maicord {
u32 host_iana_timezone(Slice<char> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    output[0] = 0;
    char name[kIanaTimezoneMaximumBytes + 1]{};
    const u32 length = local_zone_name({name, sizeof(name)});
    if (!length || length > kIanaTimezoneMaximumBytes || length >= output.count)
        return 0;
    for (u32 i = 0; i <= length; ++i)
        output[i] = name[i];
    return length;
}
} // namespace maicord

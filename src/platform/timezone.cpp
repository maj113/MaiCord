#include "maicord/platform/timezone.hpp"
#include <windows.h>

namespace maicord {
u32 host_iana_timezone(Slice<char> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    output[0] = 0;
    // Windows supplies ICU from 1903 onward. Resolve it from System32 only;
    // older/missing components leave this optional metadata absent.
    HMODULE module = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return 0;
    using GetTimezone = i32(__cdecl*)(u16*, i32, i32*);
    const auto query = reinterpret_cast<GetTimezone>(GetProcAddress(module, "ucal_getDefaultTimeZone"));
    u16 wide[kIanaTimezoneMaximumBytes + 1]{};
    i32 status = 0;
    const i32 length = query ? query(wide, kIanaTimezoneMaximumBytes + 1, &status) : 0;
    FreeLibrary(module);
    if (status > 0 || length <= 0 || length > static_cast<i32>(kIanaTimezoneMaximumBytes) ||
        static_cast<u32>(length) >= output.count)
        return 0;
    for (i32 n = 0; n < length; ++n) {
        const u16 c = wide[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
              c == '_' || c == '-' || c == '+'))
            return 0;
    }
    for (i32 n = 0; n < length; ++n)
        output[n] = static_cast<char>(wide[n]);
    output[length] = 0;
    if (utf8_equal({output.data, static_cast<u32>(length)}, utf8_literal("Etc/Unknown"))) {
        output[0] = 0;
        return 0;
    }
    return static_cast<u32>(length);
}
} // namespace maicord

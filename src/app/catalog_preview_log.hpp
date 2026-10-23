#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/protocol/cdn_url.hpp"

namespace maicord::app_detail {

// One warning line for a GIF picker tile (a favourite or catalog result) that
// shows its placeholder instead of its preview: the stage that refused it, the
// preview format and reason codes, and the source's host and extension. The
// source's path and query (signed tokens) are never logged.
struct CatalogPreviewLog {
    char text[320];
    u32 bytes = 0;
    __declspec(noinline) explicit CatalogPreviewLog(const char* stage) noexcept {
        add("catalog_preview_fallback stage=");
        add(stage);
    }
    __declspec(noinline) CatalogPreviewLog& add(const char* value) noexcept {
        while (*value && bytes + 1 < sizeof(text))
            text[bytes++] = *value++;
        return *this;
    }
    __declspec(noinline) CatalogPreviewLog& field(const char* key, u64 value) noexcept {
        char digits[20];
        const u32 count = decimal_write(digits, value);
        add(" ");
        add(key);
        add("=");
        for (u32 i = 0; i < count && bytes + 1 < sizeof(text); ++i)
            text[bytes++] = digits[i];
        return *this;
    }
    CatalogPreviewLog& source(Utf8View url) noexcept {
        bytes += media_url_log_fields(url, {text + bytes, static_cast<u32>(sizeof(text) - 1 - bytes)});
        return *this;
    }
    void write() noexcept {
        text[bytes] = 0;
        log_write(LogLevel::warning, "media", text);
    }
};

} // namespace maicord::app_detail

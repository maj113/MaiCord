#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
inline constexpr u32 kClientThemeDocumentMaximumBytes = 64u * 1024u;
inline constexpr u32 kClientThemeNameMaximumBytes = 128;
inline constexpr u32 kClientThemeDocumentMaximumDepth = 8;
enum class ClientThemeBase : u8 {
    dark,
    light,
    amoled
};
enum class ClientThemeDocumentError : u8 {
    none,
    invalid_input,
    capacity,
    json,
    duplicate_field,
    missing_version,
    unsupported_version,
    type,
    range,
    text,
    reference,
    cycle
};
struct ClientThemeDocument {
    Theme theme{};
    UiThemeTypography typography{};
    UiThemeMetrics metrics{};
    char name[kClientThemeNameMaximumBytes + 1]{};
    u32 name_bytes{};
    ClientThemeBase base{ClientThemeBase::dark};
    // Component tier: zero colours keep their semantic-derived defaults.
    UiThemeIcons icons{};
    UiThemeSettings settings{};
};
struct ClientThemeDocumentResult {
    ClientThemeDocumentError error{};
    u32 error_offset{}, unsupported_fields{};
    bool ok() const noexcept {
        return error == ClientThemeDocumentError::none;
    }
};
ClientThemeDocument client_theme_document_defaults(ClientThemeBase = ClientThemeBase::dark) noexcept;
bool client_theme_document_valid(const ClientThemeDocument&) noexcept;
bool client_theme_document_equal(const ClientThemeDocument&, const ClientThemeDocument&) noexcept;
// Partial fields inherit defaults for the same base; a changed base starts its
// built-in defaults before any overrides, independent of member order.
// The owned candidate is published only
// after complete indexed JSON/schema validation. Future token names are counted
// without applying them. Scratch belongs to the caller; no allocation or IO.
ClientThemeDocumentResult client_theme_document_parse(Utf8View, const ClientThemeDocument& defaults,
                                                      Slice<u32> structural_scratch,
                                                      ClientThemeDocument* output) noexcept;
// Canonical version 2.0 document contains every resolved typed value. Invalid
// input or insufficient capacity preserves output and bytes; null/zero output
// is a supported size query, updating bytes only on success.
ClientThemeDocumentResult client_theme_document_write(const ClientThemeDocument&, Slice<char> output,
                                                      u32* bytes) noexcept;
} // namespace maicord

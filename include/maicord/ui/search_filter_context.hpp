#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {
inline constexpr u32 kUiSearchFilterQueryBytes = 256;
inline constexpr u32 kUiSearchFilterChoiceMaximum = 10;
inline constexpr u16 kUiSearchFilterNoSelection = 0xffff;
enum class UiSearchFilterKind : u8 {
    none,
    from,
    in,
    mentions,
    has,
    pinned,
    author_type,
    file_type,
    link_from,
    file_name
};

// Ranges are UTF-8 byte offsets. Decoded bytes are owned by this value, so
// copying a context never keeps pointers into another context or edit.
struct UiSearchFilterContext {
    char decoded[kUiSearchFilterQueryBytes]{};
    u32 token_begin{}, token_end{}, value_begin{}, value_end{};
    u16 value_bytes{}, prefix_bytes{};
    u16 same_kind_occurrences{}, other_value_count{};
    UiSearchFilterKind kind{};
    bool quoted{}, quote_complete{}, has_value{};
    Utf8View value() const noexcept {
        return {decoded, value_bytes};
    }
    Utf8View prefix() const noexcept {
        return {decoded, prefix_bytes};
    }
};
static_assert(sizeof(UiSearchFilterContext) <= 320);

// Tolerates an unfinished quoted value; invalid input/UTF-8/endpoints and
// selections crossing a value return an entirely empty context. The prefix is
// decoded only through the caret (an unfinished escape pair is excluded);
// replacement always covers the whole value, including quote delimiters.
// Counts are lexical occurrences, not distinct IDs: callers must still run full
// query validation and identity deduplication when committing.
// other_value_count excludes this value and empty values; four populated other
// values conservatively prevent introducing another identity filter.
UiSearchFilterContext ui_search_filter_context(Utf8View query, u32 anchor, u32 caret) noexcept;

struct UiSearchFilterChoiceView {
    u64 id{};
    u32 semantic{};
    Utf8View label{}, detail{}, value{};
    // from:/mentions: people rows: the user's avatar hash (id is the user).
    Utf8View avatar_hash{};
};
struct UiSearchFilterChoiceSource {
    void* context{};
    ConstSlice<UiSearchFilterChoiceView> rows{};
    u32 revision{};
    u16 selected = kUiSearchFilterNoSelection;
    Utf8View heading{}, hint{};
    bool (*current)(void*) noexcept = nullptr;
    UiSearchFilterKind kind{};
};
// The host owns rows and strings until it replaces this revision. Consumers
// accept at most ChoiceMaximum rows and validate all targets by revision/current;
// current is a read-only check, never a matching callback.
// Replacements are value-only. The Forward label emits snapshot, which
// the query parser accepts as its canonical search spelling.
ConstSlice<UiSearchFilterChoiceView> ui_search_filter_fixed_choices(UiSearchFilterKind kind) noexcept;
} // namespace maicord

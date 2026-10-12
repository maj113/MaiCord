#pragma once
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/spell_tokenize.hpp"
#include "maicord/ui/text_composition.hpp"

namespace maicord {

// Composer spell checking off the input path. Edits schedule a debounced pass
// from the frame tick; each pass re-tokenizes the text (cheap) and queries the
// backend only for unchecked words absent from a small hash cache, within a
// per-tick budget. A byte frontier preserves progress across cache eviction.
// The word holding the caret
// is skipped while it is still being typed (until the caret leaves it).
inline constexpr u32 kComposerSpellcheckCacheCapacity = 512; // power of two
inline constexpr u32 kComposerSpellcheckSpanCapacity = 32;
inline constexpr u32 kComposerSpellcheckSuggestions = 5;
inline constexpr u32 kComposerSpellcheckSuggestionBytes = kComposerSpellcheckSuggestions * kSpellTokenMaximumBytes;
inline constexpr u64 kComposerSpellcheckDebounceMs = 300;
inline constexpr u64 kComposerSpellcheckContinueMs = 8;
inline constexpr u32 kComposerSpellcheckTickBudget = 16;
// Popup action ids, disjoint from TextEditCommand (1..7).
inline constexpr u32 kComposerSpellcheckActionBase = 0x5C00;
inline constexpr u32 kComposerSpellcheckActionAdd = kComposerSpellcheckActionBase + kComposerSpellcheckSuggestions;
inline constexpr u32 kComposerSpellcheckActionIgnore = kComposerSpellcheckActionAdd + 1;
// Suggestions (or one disabled "No suggestions" row), Add to dictionary, Ignore.
inline constexpr u32 kComposerSpellcheckMenuItems = kComposerSpellcheckSuggestions + 2;

enum class ComposerSpellVerdict : u8 { unavailable, correct, misspelled };

struct ComposerSpellcheckBackend {
    void* context;
    ComposerSpellVerdict (*check)(void* context, Utf8View word) noexcept;
    u32 (*suggest)(void* context, Utf8View word, Slice<char> storage, Slice<Utf8View> output) noexcept;
    // add: the user dictionary; otherwise ignore for the session.
    bool (*learn)(void* context, Utf8View word, bool add) noexcept;
};

struct ComposerSpellcheckDocument {
    const void* model;
    u64 identity;
    u32 text_revision;
    u32 caret;
    Utf8View text;
};

struct ComposerSpellcheck {
    ComposerSpellcheckBackend backend;
    u64 cache[kComposerSpellcheckCacheCapacity];
    TextCompositionSpan spans[kComposerSpellcheckSpanCapacity]; // UTF-16 positions in model text
    u64 span_hashes[kComposerSpellcheckSpanCapacity]; // durable verdicts for visible marks
    char suggestion_text[kComposerSpellcheckSuggestionBytes];
    Utf8View suggestions[kComposerSpellcheckSuggestions];
    const void* model;
    u64 identity;
    u64 due_at_ms;
    u32 text_revision, caret;
    u32 cache_count, span_count, suggestion_count;
    u32 revision;            // bumps when spans change
    u32 span_text_revision;  // model text revision the spans describe
    u32 checked_through;     // first byte that can still need a backend check
    u32 queries;             // backend checks issued
    u32 menu_first, menu_last, menu_text_revision;
    bool observed, caret_word_pending, refresh, unavailable, menu_word;
    bool saved_marks_may_be_evicted; // false: all live saved hashes remain in cache
};

// Returns true when spans changed. Disabled or unusable clears spans and never
// touches the backend.
bool composer_spellcheck_tick(ComposerSpellcheck* spellcheck, const ComposerSpellcheckDocument& document, bool enabled,
                              u64 now_ms) noexcept;
constexpr u64 composer_spellcheck_next_wake(const ComposerSpellcheck& spellcheck) noexcept {
    return spellcheck.refresh ? 1 : spellcheck.due_at_ms;
}
constexpr ConstSlice<TextCompositionSpan> composer_spellcheck_spans(const ComposerSpellcheck& spellcheck) noexcept {
    return {spellcheck.spans, spellcheck.span_count};
}
// Menu rows for a flagged word at offset; 0 when there is none. Labels borrow
// the spellcheck's suggestion storage, valid until the next build.
u32 composer_spellcheck_menu_build(ComposerSpellcheck* spellcheck, Utf8View text, u32 text_revision, u32 offset,
                                   Slice<PopupItem> items) noexcept;
constexpr bool composer_spellcheck_menu_action(u32 action) noexcept {
    return action >= kComposerSpellcheckActionBase && action <= kComposerSpellcheckActionIgnore;
}
enum class ComposerSpellcheckEffect : u8 { none, replace, learned };
struct ComposerSpellcheckActivation {
    ComposerSpellcheckEffect effect;
    u32 first, last;
    Utf8View replacement;
};
// A stale text revision yields none. Learning marks the word correct and
// schedules a refresh pass.
ComposerSpellcheckActivation composer_spellcheck_menu_activate(ComposerSpellcheck* spellcheck, Utf8View text,
                                                               u32 text_revision, u32 action) noexcept;

} // namespace maicord

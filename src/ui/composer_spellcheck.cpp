#include "maicord/ui/composer_spellcheck.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/menu_row.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
static_assert((kComposerSpellcheckCacheCapacity & (kComposerSpellcheckCacheCapacity - 1)) == 0);
constexpr u64 kVerdictMask = 3;
constexpr u64 kCorrect = 1, kMisspelled = 2;

// Entry: word hash with the low two bits replaced by the verdict; 0 is empty.
u64 key(Utf8View word) noexcept {
    return hash64_counted_bytes(0x9E3779B97F4A7C15ull, word.data, word.bytes) & ~kVerdictMask;
}
u64 lookup(const ComposerSpellcheck& s, u64 hash) noexcept {
    for (u32 probe = 0, slot = static_cast<u32>(hash >> 32) & (kComposerSpellcheckCacheCapacity - 1);
         probe < kComposerSpellcheckCacheCapacity; ++probe, slot = (slot + 1) & (kComposerSpellcheckCacheCapacity - 1)) {
        const u64 entry = s.cache[slot];
        if (!entry)
            return 0;
        if ((entry & ~kVerdictMask) == hash)
            return entry & kVerdictMask;
    }
    return 0;
}
void clear_verdicts(ComposerSpellcheck* s) noexcept {
    clear_bytes(s->cache, sizeof(s->cache));
    s->cache_count = 0;
}
void store(ComposerSpellcheck* s, u64 hash, u64 verdict) noexcept {
    // Past 3/4 load the table restarts; verdicts are cheap to re-query.
    const bool cleared = s->cache_count >= kComposerSpellcheckCacheCapacity / 4 * 3;
    if (cleared) {
        clear_verdicts(s);
        s->saved_marks_may_be_evicted = true;
    }
    for (u32 slot = static_cast<u32>(hash >> 32) & (kComposerSpellcheckCacheCapacity - 1);;
         slot = (slot + 1) & (kComposerSpellcheckCacheCapacity - 1)) {
        u64& entry = s->cache[slot];
        if (entry && (entry & ~kVerdictMask) != hash)
            continue;
        if (!entry)
            ++s->cache_count;
        entry = hash | verdict;
        return;
    }
}

// Running UTF-8 byte -> UTF-16 position conversion over increasing offsets.
struct Utf16Cursor {
    Utf8View text;
    u32 byte, unit;
    u32 advance(u32 target) noexcept {
        while (byte < target && byte < text.bytes) {
            const u8 lead = static_cast<u8>(text.data[byte]);
            const u32 width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            unit += width == 4 ? 2 : 1;
            byte += width;
        }
        return unit;
    }
};

struct PassResult {
    bool changed, pending;
};
bool marked(const ComposerSpellcheck& s, u64 hash, u32 first, u32 last) noexcept {
    for (u32 i = 0; i < s.span_count; ++i) {
        if (s.spans[i].start > first)
            break;
        if (s.spans[i].start == first && s.spans[i].end == last && s.span_hashes[i] == (hash | kMisspelled))
            return true;
    }
    return false;
}
bool saved_misspelling(const ComposerSpellcheck& s, u64 hash) noexcept {
    for (u32 i = 0; i < s.span_count; ++i)
        if (s.span_hashes[i] == (hash | kMisspelled))
            return true;
    return false;
}
PassResult pass(ComposerSpellcheck* s, const ComposerSpellcheckDocument& document, u32 budget,
                bool retain_marks = true) noexcept {
    TextCompositionSpan next[kComposerSpellcheckSpanCapacity]{};
    u64 next_hashes[kComposerSpellcheckSpanCapacity]{};
    u32 count = 0;
    u32 first_unchecked = document.text.bytes;
    bool pending = false;
    Utf16Cursor units{document.text, 0, 0};
    auto tokenizer = spell_tokenizer(document.text);
    SpellToken token{};
    while (spell_tokenizer_next(&tokenizer, &token)) {
        if (s->caret_word_pending && document.caret >= token.first && document.caret <= token.last)
            continue;
        const Utf8View word{document.text.data + token.first, token.last - token.first};
        u32 first = 0, last = 0;
        bool positions = false;
        const u64 hash = key(word);
        u64 verdict = lookup(*s, hash);
        if (!verdict && s->saved_marks_may_be_evicted && saved_misspelling(*s, hash)) {
            if (!retain_marks) {
                verdict = kMisspelled;
            } else {
                first = units.advance(token.first); last = units.advance(token.last); positions = true;
                if (marked(*s, hash, first, last))
                    verdict = kMisspelled;
            }
        }
        if (!verdict && retain_marks && token.first < s->checked_through)
            verdict = kCorrect;
        if (!verdict) {
            if (s->unavailable)
                break;
            if (!budget) {
                if (!pending)
                    first_unchecked = token.first;
                pending = true;
                continue;
            }
            --budget;
            ++s->queries;
            const auto checked = s->backend.check(s->backend.context, word);
            if (checked == ComposerSpellVerdict::unavailable) {
                s->unavailable = true;
                count = 0;
                pending = false;
                first_unchecked = 0;
                break;
            }
            verdict = checked == ComposerSpellVerdict::misspelled ? kMisspelled : kCorrect;
            store(s, hash, verdict);
        }
        if (verdict != kMisspelled || count == kComposerSpellcheckSpanCapacity)
            continue;
        if (!positions) {
            first = units.advance(token.first);
            last = units.advance(token.last);
        }
        next_hashes[count] = hash | kMisspelled;
        auto& span = next[count++];
        span.start = first;
        span.end = last;
        span.line_style = TextCompositionLineStyle::squiggle;
        span.attribute = TextCompositionAttribute::input_error;
    }
    s->span_text_revision = document.text_revision;
    s->checked_through = retain_marks ? first_unchecked : 0;
    const bool changed = count != s->span_count || !bytes_equal(next, s->spans, count * sizeof(next[0]));
    if (changed) {
        copy_bytes(s->spans, next, count * sizeof(next[0]));
        s->span_count = count;
    }
    copy_bytes(s->span_hashes, next_hashes, sizeof(next_hashes));
    if (!count)
        s->saved_marks_may_be_evicted = false;
    return {changed, pending};
}
bool clear_spans(ComposerSpellcheck* s) noexcept {
    const bool changed = s->span_count != 0;
    s->span_count = 0;
    s->saved_marks_may_be_evicted = false;
    return changed;
}
} // namespace

bool composer_spellcheck_tick(ComposerSpellcheck* s, const ComposerSpellcheckDocument& document, bool enabled,
                              u64 now_ms) noexcept {
    if (!enabled || !s->backend.check || s->unavailable || !document.model) {
        s->due_at_ms = 0;
        s->refresh = false;
        s->observed = false;
        s->checked_through = 0;
        const bool changed = clear_spans(s);
        if (changed)
            ++s->revision;
        return changed;
    }
    const bool fresh = !s->observed || document.model != s->model || document.identity != s->identity;
    const bool edited = fresh || document.text_revision != s->text_revision;
    const bool moved = !edited && document.caret != s->caret;
    s->observed = true;
    s->model = document.model;
    s->identity = document.identity;
    s->text_revision = document.text_revision;
    s->caret = document.caret;
    bool changed = false;
    if (edited) {
        // A loaded draft has no word in progress; typing defers its own word.
        s->caret_word_pending = !fresh;
        s->due_at_ms = fresh ? now_ms : now_ms + kComposerSpellcheckDebounceMs;
        s->checked_through = 0;
        changed = pass(s, document, 0, false).changed; // keep known marks aligned with the text
    } else if (moved && s->caret_word_pending) {
        s->caret_word_pending = false;
        s->checked_through = 0;
        s->due_at_ms = now_ms;
    }
    if (s->refresh) {
        s->refresh = false;
        s->checked_through = 0;
        if (!s->due_at_ms)
            s->due_at_ms = now_ms;
    }
    if (s->due_at_ms && now_ms >= s->due_at_ms) {
        const auto result = pass(s, document, kComposerSpellcheckTickBudget);
        changed = changed || result.changed;
        s->due_at_ms = result.pending ? now_ms + kComposerSpellcheckContinueMs : 0;
    }
    if (changed)
        ++s->revision;
    return changed;
}

static bool word_at(const ComposerSpellcheck& s, Utf8View text, u32 offset, SpellToken* word,
                    bool retain_marks) noexcept {
    if (!s.span_count)
        return false;
    auto tokenizer = spell_tokenizer(text);
    Utf16Cursor units{text, 0, 0};
    SpellToken token{};
    while (spell_tokenizer_next(&tokenizer, &token)) {
        if (token.first > offset)
            return false;
        if (offset > token.last)
            continue;
        const u64 hash = key({text.data + token.first, token.last - token.first});
        const u64 verdict = lookup(s, hash);
        if (verdict != kMisspelled) {
            const u32 first = units.advance(token.first), last = units.advance(token.last);
            if (verdict || !retain_marks || !marked(s, hash, first, last))
                return false;
        }
        *word = token;
        return true;
    }
    return false;
}

u32 composer_spellcheck_menu_build(ComposerSpellcheck* s, Utf8View text, u32 text_revision, u32 offset,
                                   Slice<PopupItem> items) noexcept {
    s->menu_word = false;
    s->suggestion_count = 0;
    SpellToken word{};
    if (!word_at(*s, text, offset, &word, text_revision == s->span_text_revision))
        return 0;
    const Utf8View value{text.data + word.first, word.last - word.first};
    if (s->backend.suggest)
        s->suggestion_count = minimum(
            s->backend.suggest(s->backend.context, value, {s->suggestion_text, sizeof(s->suggestion_text)},
                               {s->suggestions, kComposerSpellcheckSuggestions}),
            kComposerSpellcheckSuggestions);
    s->menu_word = true;
    s->menu_first = word.first;
    s->menu_last = word.last;
    s->menu_text_revision = text_revision;
    u32 count = 0;
    for (u32 i = 0; i < s->suggestion_count; ++i)
        items[count++] = {s->suggestions[i], kComposerSpellcheckActionBase + i, popup_item_enabled};
    if (!count)
        items[count++] = {utf8_literal("No suggestions"), kComposerSpellcheckActionBase, popup_item_none};
    items[count] = {utf8_literal("Add to dictionary"), kComposerSpellcheckActionAdd,
                    static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    items[count++].presentation = ui_menu_row_presentation(UiIcon::check);
    items[count] = {utf8_literal("Ignore"), kComposerSpellcheckActionIgnore, popup_item_enabled};
    items[count++].presentation = ui_menu_row_presentation(UiIcon::eye_off);
    return count;
}

ComposerSpellcheckActivation composer_spellcheck_menu_activate(ComposerSpellcheck* s, Utf8View text,
                                                               u32 text_revision, u32 action) noexcept {
    if (!composer_spellcheck_menu_action(action) || !s->menu_word || s->menu_text_revision != text_revision ||
        s->menu_last > text.bytes || s->menu_first >= s->menu_last)
        return {};
    s->menu_word = false;
    const u32 first = s->menu_first, last = s->menu_last;
    const Utf8View word{text.data + first, last - first};
    if (action < kComposerSpellcheckActionAdd) {
        const u32 index = action - kComposerSpellcheckActionBase;
        if (index >= s->suggestion_count)
            return {};
        return {ComposerSpellcheckEffect::replace, first, last, s->suggestions[index]};
    }
    if (s->backend.learn)
        s->backend.learn(s->backend.context, word, action == kComposerSpellcheckActionAdd);
    const u64 hash = key(word);
    for (u32 i = 0; i < s->span_count; ++i)
        if (s->span_hashes[i] == (hash | kMisspelled))
            s->span_hashes[i] = 0;
    store(s, hash, kCorrect);
    s->refresh = true;
    return {ComposerSpellcheckEffect::learned, first, last, {}};
}

} // namespace maicord

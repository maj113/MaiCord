#include "application_composer_spellcheck.hpp"
#include "maicord/core/memory.hpp"
#include "application_state.hpp"
#include "maicord/platform/spell_checker.hpp"

namespace maicord::app_detail {
namespace {
SpellChecker g_checker{};

ComposerSpellVerdict check(void*, Utf8View word) noexcept {
    switch (spell_checker_check(&g_checker, word)) {
    case SpellCheckerVerdict::misspelled:
        return ComposerSpellVerdict::misspelled;
    case SpellCheckerVerdict::correct:
        return ComposerSpellVerdict::correct;
    default:
        return ComposerSpellVerdict::unavailable;
    }
}
u32 suggest(void*, Utf8View word, Slice<char> storage, Slice<Utf8View> output) noexcept {
    return spell_checker_suggest(&g_checker, word, storage, output);
}
bool learn(void*, Utf8View word, bool add) noexcept {
    return add ? spell_checker_add(&g_checker, word) : spell_checker_ignore(&g_checker, word);
}
} // namespace

void application_composer_spellcheck_tick(u64 now_ms) noexcept {
    auto& spell = g_app.composer_spellcheck;
    if (!spell.backend.check)
        spell.backend = {nullptr, check, suggest, learn};
    auto& scene = g_app.message_composer_scene;
    const MessageComposer* model = scene.active ? scene.model : nullptr;
    ComposerSpellcheckDocument document{};
    if (model && model->text)
        document = {model, model->document_identity, model->text_revision, model->caret,
                    message_composer_text(*model)};
    const bool enabled = g_app.preferences.spellcheck && g_app.account_ui && !g_app.login.visible;
    const bool changed = composer_spellcheck_tick(&spell, document, enabled, now_ms);
    const auto spans = composer_spellcheck_spans(spell);
    // Visible marks follow every text revision; an empty set needs no frame.
    const bool redraw = changed || (spans.count && scene.spelling_text_revision != spell.span_text_revision);
    scene.spelling = spans;
    scene.spelling_text_revision = spell.span_text_revision;
    if (!redraw)
        return;
    ++scene.composition_revision;
    application_request_frame();
}

void application_composer_spellcheck_release() noexcept {
    reset_value(&g_app.message_composer_scene.spelling);
    reset_value(&g_app.composer_spellcheck.backend);
    g_app.composer_spellcheck.span_count = 0;
    spell_checker_release(&g_checker);
}

} // namespace maicord::app_detail

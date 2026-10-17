#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

namespace maicord {
namespace text_emitter_probe {
#include "text_projection_fixture.inc"
struct Comparison {
    const UiTextCommandStream* expected;
    u32 calls = 0, fail_at = ~0u;
    bool equal = true;
};
bool consume(void* context, const UiTextCommand& command, u32 ordinal) noexcept {
    auto& c = *static_cast<Comparison*>(context);
    ++c.calls;
    if (ordinal == c.fail_at) return false;
    if (!c.expected || ordinal >= c.expected->count) { c.equal = false; return false; }
    const auto& e = c.expected->storage[ordinal];
    c.equal &= ordinal + 1 == c.calls && utf8_equal(command.text, e.text) &&
        command.owner.manager == e.owner.manager && command.owner.handle.slot == e.owner.handle.slot &&
        command.owner.handle.generation == e.owner.handle.generation &&
        ui_rect_equal(command.rectangle, e.rectangle) && ui_rect_equal(command.clip, e.clip) &&
        bytes_equal(&command.color, &e.color, sizeof(Color)) && command.style_flags == e.style_flags &&
        command.horizontal_alignment == e.horizontal_alignment && command.vertical_alignment == e.vertical_alignment &&
        command.wrapping == e.wrapping && command.overflow == e.overflow && command.face == e.face &&
        command.effect.kind == e.effect.kind && command.effect.count == e.effect.count &&
        command.effect.animate == e.effect.animate && command.effect.started_ms == e.effect.started_ms &&
        bytes_equal(command.effect.colors, e.effect.colors, sizeof(e.effect.colors)) &&
        command.styles.count == e.styles.count &&
        bytes_equal(command.styles.data, e.styles.data, e.styles.count * sizeof(TextStyleSpan));
    return c.equal;
}
UiTextCommand sample() noexcept {
    auto& f = fixture;
    const auto* home = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::home_button));
    return {utf8_literal("sample"), {&f.shell.widgets, home->widget}, home->rectangle, home->rectangle,
        kDefaultTheme.text_primary, text_style_bold, UiTextAlignment::center, UiTextAlignment::end,
        UiTextWrapping::no_wrap, UiTextOverflow::ellipsis, UiTextFace::normal, {}, {f.styles, 2}};
}
bool transient_projection(UiTextOutput* output) noexcept {
    for (u32 index = 0; index < 3; ++index) {
        char label[]{static_cast<char>('A' + index), 'x', 'y'};
        auto command = sample(); command.text = {label, 3};
        UiTextEffect effect{}; effect.kind = 3; effect.count = 8; effect.animate = true;
        effect.started_ms = 100 + index;
        for (u32 color = 0; color < 8; ++color) effect.colors[color] = 0x112233u + color + index;
        command.effect = effect;
        if (!ui_text_output_push_owned(output, command)) return false;
        label[0] = label[1] = label[2] = '!';
    }
    const auto command = sample();
    return ui_mention_badge_text_output(1000, *command.owner.manager, command.owner.handle,
        command.rectangle, command.clip, output);
}
} // namespace text_emitter_probe

void test_text_emitter(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace text_emitter_probe;
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized, "text emitter: real tall shell and Friends scene initialize");
    if (!initialized) { ui_friends_scene_release(&f.friends); ui_scene_release(&f.shell); return; }
    f.styles[0] = {0, 1, text_style_bold}; f.styles[1] = {1, 2, text_style_subtext};
    UiTextOutput stored{&f.stream, nullptr};
    check(transient_projection(&stored), "text emitter: stored adapter owns transient strings and inline effects");
    Comparison comparison{&f.stream}; UiTextEmitter emitter{};
    UiTextOutput emitted{nullptr, &emitter};
    check(ui_text_emitter_initialize(&emitter, consume, &comparison) && transient_projection(&emitted) &&
        comparison.equal && comparison.calls == f.stream.count && emitter.signature == f.stream.signature,
        "text emitter: synchronous transient text styles effects and numeric labels equal stored commands in order");
    UiTextEmitter preflight{}; UiTextOutput checked{nullptr, &preflight};
    check(ui_text_emitter_initialize(&preflight) && transient_projection(&checked) &&
        preflight.signature == emitter.signature && preflight.count == emitter.count,
        "text emitter: callback-free structural preflight preserves complete ordered signature");
    auto invalid = sample(); invalid.owner.handle.generation = 0;
    const auto signature = preflight.signature; const auto count = preflight.count;
    check(!ui_text_output_push(&checked, invalid) && preflight.count == count &&
        preflight.signature == signature && preflight.rejected == 1,
        "text emitter: malformed later owner rejects without publishing partial signature or ordinal");
    comparison = {&f.stream, 0, 1};
    check(ui_text_emitter_initialize(&emitter, consume, &comparison) && !transient_projection(&emitted) &&
        emitter.count == 1 && emitter.rejected == 1 && comparison.calls == 2,
        "text emitter: consumer failure stops at the failed command and keeps only successful prefix");
    check(ui_text_emitter_initialize(&preflight, nullptr, nullptr, 1) &&
        !transient_projection(&checked) && preflight.count == 1 && preflight.rejected == 1,
        "text emitter: explicit caller bound rejects before unbounded projection");
    ui_text_output_reset(&checked);
    auto invisible = sample(); invisible.clip.right = invisible.clip.left;
    check(ui_text_output_push(&checked, invisible) && !preflight.count && !preflight.rejected,
        "text emitter: valid clipped overscan consumes no ordinal or capacity");
    UiTextOutput ambiguous{&f.stream, &emitter};
    check(!ui_text_output_valid(&ambiguous) && !ui_text_output_push(&ambiguous, sample()),
        "text emitter: mutually exclusive stored and synchronous destinations enforced");
    const auto resolutions = f.resolutions;
    ui_text_command_stream_reset(&f.stream);
    // Friend row More actions are icon buttons and emit no text commands.
    check(tall_projection(&stored) && f.stream.count > 1000 && f.stream.count <= 1500,
        "text emitter: supported 8192-DIP pending Friends with populated navigation fits the stream bank");
    comparison = {&f.stream};
    check(ui_text_emitter_initialize(&emitter, consume, &comparison) && tall_projection(&emitted) &&
        comparison.equal && comparison.calls == f.stream.count && emitter.count == f.stream.count &&
        emitter.signature == f.stream.signature && f.resolutions == resolutions,
        "text emitter: tall real production producers match stored stream without extra source resolution or retained copy");
    ++f.source.revision;
    check(ui_text_emitter_initialize(&preflight) && !tall_projection(&checked),
        "text emitter: stale Friends source is rejected during structural preflight");
    char composer_storage[64]{};
    MessageComposer composer{};
    MessageComposerScene composer_scene{};
    bool semantic_composer =
        message_composer_initialize(&composer, {composer_storage, sizeof(composer_storage)}) &&
        message_composer_scene_initialize_owned(&composer_scene);
    Theme composer_themes[]{kDefaultTheme, kLightTheme, kDefaultTheme, kLightTheme};
    composer_themes[2].background = background_theme_defaults();
    composer_themes[3].background = background_theme_defaults();
    bool placeholder_signature = true;
    for (auto& theme : composer_themes) {
        theme.text_secondary = {.13f, .47f, .29f, .61f};
        for (u32 mode = 0; mode < 3; ++mode) {
            message_composer_reset(&composer, 80, false);
            if (mode == 1)
                semantic_composer &= message_composer_insert_utf8(&composer, utf8_literal("draft")) ==
                                     MessageComposerResult::changed;
            if (mode == 2)
                composer.phase = MessageComposerPhase::failed;
            semantic_composer &=
                message_composer_scene_update(
                    &composer_scene, &composer,
                    {0, 0, 304, message_composer_scene_preferred_height(composer, 304, false, false)}, true)
                    .ok() &&
                shell_text_presentation_build_output(f.shell, f.state, f.view, theme, f.navigation,
                                                     &composer_scene, nullptr, nullptr, &stored, nullptr,
                                                     &f.rail);
            const auto expected = mode == 1   ? theme.text_primary
                                  : mode == 2 ? theme.notification
                                              : theme.text_secondary;
            bool found = false;
            for (u32 index = 0; index < f.stream.count; ++index) {
                const auto& command = f.commands[index];
                if (command.owner.manager == &composer_scene.widgets &&
                    command.owner.handle.slot == composer_scene.edit.slot &&
                    command.owner.handle.generation == composer_scene.edit.generation) {
                    const auto expected_text = mode == 1
                                                   ? utf8_literal("draft")
                                                   : message_composer_scene_placeholder(composer_scene);
                    found = utf8_equal(command.text, expected_text) &&
                            bytes_equal(&command.color, &expected, sizeof(Color));
                }
            }
            semantic_composer &= found;
            if (mode == 0) {
                const auto original_signature = f.stream.signature;
                Theme recolored = theme;
                recolored.text_secondary.green = .48f;
                placeholder_signature &=
                    shell_text_presentation_build_output(f.shell, f.state, f.view, recolored, f.navigation,
                                                         &composer_scene, nullptr, nullptr, &stored,
                                                         nullptr, &f.rail) &&
                    f.stream.signature != original_signature;
            }
        }
    }
    check(semantic_composer, "text emitter: actual shell composer preserves custom secondary RGBA for "
                             "flat/active Dark/Light placeholders and primary/error draft semantics");
    check(placeholder_signature, "text emitter: custom secondary token changes invalidate actual shell "
                                 "placeholder command signatures");
    message_composer_scene_release(&composer_scene);
    ui_friends_scene_release(&f.friends);
    ui_scene_release(&f.shell);
}
} // namespace maicord

#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord { namespace {
u32 failures;
void check(bool ok, Utf8View name) noexcept {
    if (ok) return;
    ++failures;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), name.data, name.bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
bool equal(Utf8View a, Utf8View b) noexcept {
    if (a.bytes != b.bytes) return false;
    for (u32 i = 0; i < a.bytes; ++i) if (a.data[i] != b.data[i]) return false;
    return true;
}
bool contains(Utf8View a, Utf8View b) noexcept {
    for (u32 i = 0; b.bytes <= a.bytes && i <= a.bytes - b.bytes; ++i)
        if (equal({a.data + i, b.bytes}, b)) return true;
    return false;
}
MessageRecord records[2]; u32 order[2]; u8 text[2048];
MessageRichRecord rich_records[8]; u16 rich_order[8]; u8 rich_text[32768];
MessageSceneRowWidget rows[2], snapshot_rows[2];
UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1];
UiLayoutNode layout[2 * kMessageSceneWidgetsPerRow + 1];
MessageTextCommand commands[256]; char display[8192]; TextStyleSpan styles[256];
MessageMarkdownSemantic semantics[128]; MessageTextMeasurement measurements[8];
MessageTextSpoilerReveal reveals[16]; u32 scratch[8192];

const MessageTextCommand* command(const MessageTextPresentation& p, u16 id) noexcept {
    for (u32 i = 0; i < p.command_count; ++i) if (p.commands[i].source_id == id) return &p.commands[i];
    return nullptr;
}
MessageTextHit hit(const MessageTextCommand* c, MessageMarkdownSemanticKind kind) noexcept {
    if (c) for (u32 i = 0; i < c->semantics.count; ++i) {
        const auto& s = c->semantics[i];
        if (s.kind == kind) return {c->message_id, s.target, c->message_revision,
                                   s.source_offset, s.source_bytes, kind, true};
    }
    return {};
}
bool hidden(const MessageTextCommand* c) noexcept {
    if (c) for (u32 i = 0; i < c->styles.count; ++i)
        if (c->styles[i].flags & text_style_spoiler) return true;
    return false;
}

void run() noexcept {
    MessageStore store{}; MessageRichStore rich{}; MessageScene scene{};
    UiVariableList list{}; MessageTimeline timeline{}; u32 units[2]{}; u64 fenwick[2]{};
    constexpr Utf8View outer = utf8_literal("||outer secret|| **outer** [outside](https://outer.example) <:outer:22>");
    constexpr Utf8View frozen = utf8_literal("||snapshot secret|| **frozen \xC5\xBE** [inside](https://snapshot.example/x) <:inner:23>");
    const bool ready = message_store_initialize(&store, {records, 2}, {order, 2}, {text, sizeof(text)}) &&
        message_store_upsert(&store, {91, 7, 9, 1700000000000ull, 0, outer, 0}).ok() &&
        message_store_upsert(&store, {92, 7, 10, 1700000000010ull, 0, utf8_literal("other"), 0}).ok() &&
        message_rich_store_initialize(&rich, {rich_records, 8}, {rich_order, 8}, {rich_text, sizeof(rich_text)});
    check(ready, utf8_literal("forwarded_text_model_ready")); if (!ready) return;
    const auto found = message_store_find(store, 91);
    const auto* record = message_store_resolve(store, found.handle);
    const u32 revision = record->revision;
    GatewayMessageEmbed outside{}, inside{};
    outside.description = utf8_literal("||outer field|| [outer embed](https://outer-embed.example)");
    inside.description = utf8_literal("||snapshot field|| [snapshot embed](https://snapshot-embed.example) <:field:24>");
    outside.type = inside.type = GatewayMessageEmbedType::rich;
    GatewayMessageEvent event{}; event.id = 91;
    event.fields = gateway_message_field_embeds | gateway_message_field_message_snapshots;
    event.embeds = {&outside, 1}; event.snapshot.present = true; event.snapshot.content = frozen;
    event.snapshot.fields = gateway_message_field_content | gateway_message_field_embeds;
    event.snapshot.embeds = {&inside, 1};
    check(message_rich_store_apply(&rich, 91, revision, event).ok(), utf8_literal("forwarded_text_store_apply"));
    const u32 default_units = ui_extent_units(80.0f);
    const bool arranged = ui_variable_list_initialize(&list, {units, 2}, {fenwick, 2}, 2, default_units) &&
        message_timeline_initialize(&timeline, &store, &list, default_units, ui_extent_units(900.0f), true) &&
        message_timeline_bind_rich_store(&timeline, &rich) &&
        message_scene_initialize(&scene, &timeline, {rows, 2}, {widgets, 2 * kMessageSceneWidgetsPerRow + 1},
                                 {layout, 2 * kMessageSceneWidgetsPerRow + 1}) &&
        message_scene_bind_snapshot_rows(&scene, {snapshot_rows, 2}) &&
        message_scene_update(&scene, {0, 0, 720, 900}).ok();
    check(arranged, utf8_literal("forwarded_text_scene_ready")); if (!arranged) return;
    MessageTextPresentation p{}; ChatStateStore state{}; NavigationView navigation{};
    const bool initialized = message_text_presentation_initialize(&p, {commands, 256}, {display, 8192},
        {styles, 256}, {semantics, 128}, {measurements, 8}, {reveals, 16}, {scratch, 8192});
    const bool built = initialized && message_text_presentation_build(&p, scene, state, nullptr, navigation,
                                                                     kDefaultTheme, 1700000000).ok();
    check(built, utf8_literal("forwarded_text_projection")); if (!built) return;
    const auto* body = command(p, 0xC001); const auto* inner_embed = command(p, 0xC004);
    const auto* outer_embed = command(p, 0x8004);
    const auto body_link = hit(body, MessageMarkdownSemanticKind::link);
    const auto embed_link = hit(inner_embed, MessageMarkdownSemanticKind::link);
    check(body && body->message_id == 91 && body->message_revision == revision &&
          contains(body->text, utf8_literal("frozen \xC5\xBE")), utf8_literal("forwarded_text_utf8_body_identity"));
    check(equal(message_text_presentation_hit_source(store, &rich, body_link), utf8_literal("https://snapshot.example/x")) &&
          equal(message_text_presentation_hit_source(store, &rich, embed_link), utf8_literal("https://snapshot-embed.example")) &&
          equal(message_text_presentation_hit_source(store, &rich, hit(outer_embed, MessageMarkdownSemanticKind::link)),
                utf8_literal("https://outer-embed.example")), utf8_literal("forwarded_text_exact_link_sources"));
    check(message_text_presentation_hit_source(store, body_link).bytes == 0,
          utf8_literal("forwarded_text_no_outer_fallback"));
    auto forged = body_link; forged.message_id = 92;
    forged.message_revision = message_store_resolve(store, message_store_find(store, 92).handle)->revision;
    check(message_text_presentation_hit_source(store, &rich, forged).bytes == 0,
          utf8_literal("forwarded_text_owner_guard"));
    forged = body_link; forged.source_offset = (forged.source_offset & 0xFFFF0000u) | 65535u;
    check(message_text_presentation_hit_source(store, &rich, forged).bytes == 0,
          utf8_literal("forwarded_text_byte_bounds"));
    char copied[1024]{};
    if (body) {
        const auto copy = message_text_project_selection_for_copy(*body,
            message_text_presentation_command_source(store, &rich, *body), 0, body->text.bytes, {copied, 1024});
        check(copy.ok && copy.transformed && contains({copied, copy.bytes}, utf8_literal("<:inner:23>")) &&
              !contains({copied, copy.bytes}, utf8_literal("<:outer:22>")), utf8_literal("forwarded_text_copy_emoji_source"));
    }
    bool changed = false; const auto spoiler = hit(body, MessageMarkdownSemanticKind::spoiler);
    check(hidden(body) && hidden(inner_embed) && message_text_presentation_toggle_spoiler(&p, store, &rich, spoiler, &changed) &&
          changed && message_text_presentation_build(&p, scene, state, nullptr, navigation, kDefaultTheme, 1700000000).ok() &&
          !hidden(command(p, 0xC001)) && hidden(command(p, 0xC004)) && hidden(command(p, 0x8004)),
          utf8_literal("forwarded_text_spoiler_source_isolation"));
    check(message_store_upsert(&store, {91, 7, 9, 1700000000000ull, 0, utf8_literal("changed outer"), 0}).ok(),
          utf8_literal("forwarded_text_outer_update"));
    const u32 next = message_store_resolve(store, found.handle)->revision;
    GatewayMessageEvent omitted{}; omitted.id = 91;
    check(message_rich_store_apply(&rich, 91, next, omitted).ok() &&
          message_text_presentation_hit_source(store, &rich, body_link).bytes == 0,
          utf8_literal("forwarded_text_stale_revision_guard"));
    auto current = body_link; current.message_revision = next;
    check(equal(message_text_presentation_hit_source(store, &rich, current), utf8_literal("https://snapshot.example/x")),
          utf8_literal("forwarded_text_immutable_omission"));
    omitted.fields = gateway_message_field_message_snapshots;
    check(message_rich_store_apply(&rich, 91, next, omitted).ok() &&
          message_text_presentation_hit_source(store, &rich, current).bytes == 0,
          utf8_literal("forwarded_text_explicit_removal_guard"));
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_forwarded_text_contract_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}

// The "(edited)" label of a message that carries an edit time: small muted type inline after the last glyph of its
// text, on a line of its own after the accessories of a message without text, added by an update that sets the edit
// time and gone when it is cleared; never part of a copied selection. The text pass places it, the shaped layout
// carries it (the label wraps like a word), and its semantic holds the edit time the tooltip shows.
#include "maicord/model/chat/message_reconcile.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/text/text.hpp"

#include "../../../support/test_output.hpp"
#include "../../../support/ui_scene_fixtures.hpp"

namespace maicord::edited_label_test {
namespace {
u32 failures;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}

constexpr u64 kEditedMs = 1'700'000'100'000ull;
constexpr Utf8View kText = utf8_literal("hello world");
constexpr Utf8View kEdited = utf8_literal("hello world (edited)");
constexpr Utf8View kEditedLabel = utf8_literal("(edited)");
constexpr UiRect kViewport{0, 0, 720, 900};
constexpr u32 kRowCount = 4;

MessageRecord records[kRowCount];
u32 order[kRowCount];
u8 text[2048];
MessageRichRecord rich_records[8];
u16 rich_order[8];
u8 rich_text[32768];
MessageSceneRowWidget rows[kRowCount];
UiWidget widgets[kRowCount * kMessageSceneWidgetsPerRow + 1];
UiLayoutNode layout[kRowCount * kMessageSceneWidgetsPerRow + 1];
MessageTextCommand commands[256];
char display[8192];
TextStyleSpan styles[256];
MessageMarkdownSemantic semantics[128];
MessageTextMeasurement measurements[8];
MessageTextSpoilerReveal reveals[16];
u32 scratch[8192];

struct Fixture {
    MessageStore store{};
    MessageRichStore rich{};
    MessageScene scene{};
    UiVariableList list{};
    MessageTimeline timeline{};
    u32 units[kRowCount]{};
    u64 fenwick[kRowCount]{};
    MessageTextPresentation presentation{};
    ChatStateStore state{};
    NavigationView navigation{};
};

const MessageTextCommand* content_command(const MessageTextPresentation& p, u64 message_id) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].message_id == message_id && p.commands[i].measurement_kind == MessageTextMeasurementKind::content)
            return &p.commands[i];
    return nullptr;
}
// The command the group after the accessories owns.
const MessageTextCommand* edited_tail_command(const MessageTextPresentation& p, const MessageSceneRowWidget& row) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].owner.handle.slot == row.edited.slot && p.commands[i].owner.handle.generation == row.edited.generation &&
            row.edited.slot != kInvalidUiWidgetSlot)
            return &p.commands[i];
    return nullptr;
}
const MessageMarkdownSemantic* edited_semantic(const MessageTextCommand* command) noexcept {
    if (command)
        for (u32 i = 0; i < command->semantics.count; ++i)
            if (command->semantics[i].kind == MessageMarkdownSemanticKind::edited)
                return &command->semantics[i];
    return nullptr;
}
u32 edited_semantics(const MessageTextCommand* command) noexcept {
    u32 count = 0;
    if (command)
        for (u32 i = 0; i < command->semantics.count; ++i)
            count += command->semantics[i].kind == MessageMarkdownSemanticKind::edited ? 1u : 0u;
    return count;
}
const MessageSceneRowWidget* row_of(Fixture& f, u64 message_id) noexcept {
    const auto found = message_store_find(f.store, message_id);
    return found.ok() ? message_scene_find_row(&f.scene, found.order_index) : nullptr;
}
bool rebuild(Fixture& f) noexcept {
    return message_scene_update(&f.scene, kViewport).ok() &&
           message_text_presentation_build(&f.presentation, f.scene, f.state, nullptr, f.navigation, kDefaultTheme,
                                           1'700'000'000)
               .ok();
}
MessageTextMeasurement* measurement_of(Fixture& f, u64 message_id) noexcept {
    for (u32 i = 0; i < f.presentation.measurement_count; ++i)
        if (f.presentation.measurements[i].message_id == message_id)
            return &f.presentation.measurements[i];
    return nullptr;
}
GatewayMessageEvent edit_event(u64 id, u64 channel, u64 edited_ms) noexcept {
    GatewayMessageEvent event{};
    event.id = id;
    event.channel_id = channel;
    event.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_edited_timestamp;
    event.edited_timestamp_ms = edited_ms;
    return event;
}

// Shaped text: the layout width and height, and the first hit region of the label's semantic.
struct Shaped {
    float width, height;
    u32 lines;
    TextHitRegion label;
    u32 regions;
    bool ok;
};
TextSystem font_system;
TextGlyph glyphs[1024];
TextRun runs[256];
u16 clusters[1024];
TextDecoration decorations[64];
TextHitRegion hit_regions[64];
Shaped shape(Utf8View string, ConstSlice<TextStyleSpan> spans, ConstSlice<MessageMarkdownSemantic> marks,
             float width) noexcept {
    Shaped result{};
    TextHitSpan hits[4]{};
    u32 hit_count = 0;
    for (u32 i = 0; i < marks.count && hit_count < 4; ++i)
        hits[hit_count++] = TextHitSpan{marks[i].byte_offset, marks[i].byte_count, static_cast<u16>(i), 0};
    ShapedText output{};
    if (!shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64},
                                {hit_regions, 64}))
        return result;
    result.ok = text_shape_utf8(&font_system, {string, spans, width, 4096, TextWrapping::wrap,
                                TextOverflow::clip, {hits, hit_count}}, &output)
                    .ok();
    result.width = output.width;
    result.height = output.height;
    result.lines = output.line_count;
    result.regions = output.hit_region_count;
    if (output.hit_region_count)
        result.label = output.hit_regions[0];
    shaped_text_reset(&output);
    return result;
}

// What a screen reader gets: the text the layout draws is the name of its message's node, label included.
const MessageScene* g_described_scene;
void describe_row(void*, const UiWidgetManager&, UiWidgetHandle handle, const UiWidget&, const UiLayoutNode&,
                  UiAccessWidgetInfo* info) noexcept {
    const MessageSceneRowWidget* row = message_scene_resolve(*g_described_scene, handle);
    if (row && ui_widget_handle_equal(row->widget, handle)) {
        info->key = info->item = row->message_id;
        info->role = UiAccessRole::list_item;
    }
}
UiAccessNode access_nodes[256];
char access_text[16384];
u32 access_slots[kRowCount * kMessageSceneWidgetsPerRow + 8];
u32 access_keys[512];
UiAccessTree access_tree;
u32 accessible_names_containing(Utf8View needle) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < access_tree.count; ++i)
        count += test_contains(ui_access_name(access_tree, i), needle) ? 1u : 0u;
    return count;
}
bool describe_accessibly(Fixture& f) noexcept {
    g_described_scene = &f.scene;
    bool built = ui_access_tree_initialize(&access_tree, {access_nodes, 256}, {access_text, sizeof(access_text)},
                                           {access_slots, kRowCount * kMessageSceneWidgetsPerRow + 8},
                                           {access_keys, 512}) &&
                 ui_access_tree_reset(&access_tree, utf8_literal("Window"), {0, 0, 720, 900}) &&
                 ui_access_add_widgets(&access_tree, 0, f.scene.widgets, {describe_row, nullptr, 1, false, nullptr}) !=
                     kUiAccessNone;
    for (u32 i = 0; built && i < f.presentation.command_count; ++i) {
        const MessageTextCommand& command = f.presentation.commands[i];
        ui_access_attribute_text(&access_tree, command.owner, command.text, command.rectangle);
    }
    return built && ui_access_tree_finish(&access_tree);
}

void run() noexcept {
    Fixture f;
    constexpr u64 kChannel = 7;
    const bool ready =
        message_store_initialize(&f.store, {records, kRowCount}, {order, kRowCount}, {text, sizeof(text)}) &&
        message_store_upsert(&f.store, {11, kChannel, 9, 1'700'000'000'000ull, kEditedMs, kText, 0}).ok() &&
        message_store_upsert(&f.store, {12, kChannel, 10, 1'700'000'001'000ull, 0, kText, 0}).ok() &&
        message_store_upsert(&f.store, {13, kChannel, 11, 1'700'000'002'000ull, kEditedMs, {}, 0}).ok() &&
        message_rich_store_initialize(&f.rich, {rich_records, 8}, {rich_order, 8}, {rich_text, sizeof(rich_text)});
    check(ready, "edited label: fixture stores initialize");
    if (!ready)
        return;
    GatewayMessageAttachment attachment{};
    attachment.id = 70;
    attachment.filename = utf8_literal("note.txt");
    attachment.content_type = utf8_literal("text/plain");
    attachment.url = utf8_literal("https://cdn.example/note.txt");
    GatewayMessageEvent rich_event{};
    rich_event.id = 13;
    rich_event.fields = gateway_message_field_attachments;
    rich_event.attachments = {&attachment, 1};
    const auto tail_found = message_store_find(f.store, 13);
    const u32 default_units = ui_extent_units(80.0f);
    const bool arranged =
        tail_found.ok() &&
        message_rich_store_apply(&f.rich, 13, message_store_resolve(f.store, tail_found.handle)->revision, rich_event).ok() &&
        ui_variable_list_initialize(&f.list, {f.units, kRowCount}, {f.fenwick, kRowCount}, 3, default_units) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, default_units, ui_extent_units(900.0f), true) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize(&f.scene, &f.timeline, {rows, kRowCount},
                                 {widgets, kRowCount * kMessageSceneWidgetsPerRow + 1},
                                 {layout, kRowCount * kMessageSceneWidgetsPerRow + 1}) &&
        message_text_presentation_initialize(&f.presentation, {commands, 256}, {display, sizeof(display)},
                                             {styles, 256}, {semantics, 128}, {measurements, 8}, {reveals, 16},
                                             {scratch, 8192}) &&
        rebuild(f);
    check(arranged, "edited label: scene and text presentation build");
    if (!arranged)
        return;

    // Inline: the label follows the last glyph, small and muted, with the edit time for its tooltip.
    const MessageTextCommand* edited = content_command(f.presentation, 11);
    const MessageTextCommand* plain = content_command(f.presentation, 12);
    const MessageMarkdownSemantic* mark = edited_semantic(edited);
    check(edited && utf8_equal(edited->text, kEdited), "edited label: an edited message's text ends with its label");
    check(plain && utf8_equal(plain->text, kText) && edited_semantics(plain) == 0,
          "edited label: an unedited message shows none");
    check(mark && mark->byte_offset == kText.bytes + 1 && mark->byte_count == 8 && mark->target == kEditedMs / 1000 &&
              mark->source_bytes == 0 && edited_semantics(edited) == 1,
          "edited label: one semantic covers the label and carries the edit time in seconds");
    const TextStyleSpan* span = edited && edited->styles.count ? &edited->styles[edited->styles.count - 1] : nullptr;
    check(span && mark && span->byte_offset == mark->byte_offset && span->byte_count == mark->byte_count &&
              (span->flags & text_style_subtext) && span->size == text_size_xs && edited->themed_styles,
          "edited label: the label takes the small muted style, the text before it none");
    check(edited && message_text_command_copy_end(*edited) == kText.bytes &&
              plain && message_text_command_copy_end(*plain) == plain->text.bytes,
          "edited label: a copied selection stops before the label");
    const MessageTextMeasurement* edited_measure = measurement_of(f, 11);
    check(edited_measure && edited_measure->rich_layout.edited == 0.0f,
          "edited label: inline labels add no group height");

    // Screen readers: the label is part of its message's accessible text, read as "edited".
    check(describe_accessibly(f) && accessible_names_containing(kEdited) >= 1 &&
              accessible_names_containing(kEditedLabel) >= 2 && accessible_names_containing(kText) >= 2,
          "edited label: the accessible text of edited messages ends with their label, an unedited one has none");

    // Without text: a line of its own in the group after the attachments, before the reactions.
    const MessageSceneRowWidget* tail_row = row_of(f, 13);
    const MessageTextCommand* tail = tail_row ? edited_tail_command(f.presentation, *tail_row) : nullptr;
    const UiLayoutNode* group = tail_row ? message_scene_layout_node(f.scene, tail_row->edited) : nullptr;
    const UiLayoutNode* attachments = tail_row ? message_scene_layout_node(f.scene, tail_row->attachments) : nullptr;
    const MessageTextMeasurement* tail_measure = measurement_of(f, 13);
    check(tail && utf8_equal(tail->text, kEditedLabel) && edited_semantic(tail) && !content_command(f.presentation, 13),
          "edited label: a message without text shows the label alone, in the group of its own");
    check(group && attachments && group->rectangle.top >= attachments->rectangle.bottom - 0.01f &&
              tail_measure && tail_measure->rich_layout.edited > 0.0f &&
              tail_measure->rich_item_layout.edited_height == tail_measure->rich_layout.edited,
          "edited label: that group follows the accessories and takes the line's height");
    check(tail_row && tail_row->rich_layout.total() >= tail_row->rich_layout.edited,
          "edited label: the row's rich height counts the group");

    // An update that sets the edit time adds the label; one that clears it removes it again.
    GatewayMessageEvent update = edit_event(12, kChannel, kEditedMs + 5'000);
    const bool updated = message_reconcile_update(&f.store, update, MessageContentAvailability::available).ok() &&
                         rebuild(f);
    const MessageTextCommand* now_edited = content_command(f.presentation, 12);
    const MessageMarkdownSemantic* now_mark = edited_semantic(now_edited);
    check(updated && now_edited && utf8_equal(now_edited->text, kEdited) && now_mark &&
              now_mark->target == (kEditedMs + 5'000) / 1000,
          "edited label: an update that sets the edit time adds the label with the new time");
    update = edit_event(12, kChannel, 0);
    const bool cleared = message_reconcile_update(&f.store, update, MessageContentAvailability::available).ok() &&
                         rebuild(f);
    const MessageTextCommand* now_plain = content_command(f.presentation, 12);
    check(cleared && now_plain && utf8_equal(now_plain->text, kText) && edited_semantics(now_plain) == 0,
          "edited label: an update that clears the edit time removes it");
    update = edit_event(13, kChannel, 0);
    const bool tail_cleared = message_reconcile_update(&f.store, update, MessageContentAvailability::available).ok() &&
                              rebuild(f);
    const MessageSceneRowWidget* unedited_row = row_of(f, 13);
    check(tail_cleared && unedited_row && unedited_row->edited.slot == kInvalidUiWidgetSlot &&
              !edited_tail_command(f.presentation, *unedited_row) && measurement_of(f, 13) &&
              measurement_of(f, 13)->rich_layout.edited == 0.0f,
          "edited label: a message without text loses its group when the edit time clears");
    update = edit_event(13, kChannel, kEditedMs);
    const bool tail_added = message_reconcile_update(&f.store, update, MessageContentAvailability::available).ok() &&
                            rebuild(f);
    const MessageSceneRowWidget* edited_row = row_of(f, 13);
    check(tail_added && edited_row && edited_row->edited.slot != kInvalidUiWidgetSlot &&
              edited_tail_command(f.presentation, *edited_row),
          "edited label: an update that sets the edit time of a message without text builds its group");

    // Compact rows read as one flow: the label stays inline even without text.
    MessageSceneDisplay display_mode{};
    display_mode.compact = true;
    const bool compact = message_scene_set_display(&f.scene, display_mode) && rebuild(f);
    const MessageSceneRowWidget* compact_row = row_of(f, 13);
    const MessageTextCommand* compact_label = content_command(f.presentation, 13);
    check(compact && compact_row && compact_row->edited.slot == kInvalidUiWidgetSlot && compact_label &&
              utf8_equal(compact_label->text, kEditedLabel),
          "edited label: a compact row keeps the label inline after the (empty) text");
    display_mode.compact = false;
    check(message_scene_set_display(&f.scene, display_mode) && rebuild(f) && row_of(f, 13) &&
              row_of(f, 13)->edited.slot != kInvalidUiWidgetSlot,
          "edited label: leaving compact mode brings the group back");
    message_scene_release(&f.scene);

    // The shaped layout: the label sits after the last glyph and wraps like a word when the line is full.
    if (!text_system_initialize(&font_system, L"Segoe UI", 16).ok()) {
#if defined(_WIN32)
        check(false, "edited label: the text system initializes");
#endif
        return;
    }
    const Shaped full = shape(edited->text, edited->styles, edited->semantics, 600);
    const Shaped body = shape(kText, {}, {}, 600);
    check(full.ok && body.ok && full.regions >= 1 && full.label.left >= body.width - 0.5f &&
              full.width > body.width && full.label.right <= full.width + 0.5f && full.lines == 1 &&
              full.label.right > full.label.left && full.label.bottom > full.label.top,
          "edited label: the layout puts the label after the last glyph of the text, on its line");
    const Shaped wrapped = shape(edited->text, edited->styles, edited->semantics, body.width + 12.0f);
    check(wrapped.ok && wrapped.lines == 2 && wrapped.regions >= 1 && wrapped.label.top >= body.height * 0.5f &&
              wrapped.label.left <= 1.0f,
          "edited label: a full line wraps the label like a word");
    const Shaped bare = shape(kText, {}, {}, body.width + 12.0f);
    check(bare.ok && bare.lines == 1 && bare.regions == 0, "edited label: an unedited message lays out as before");
    text_system_release(&font_system);
}
} // namespace
} // namespace maicord::edited_label_test

MAICORD_TEST_ENTRY(maicord_edited_label_entry) {
    maicord::edited_label_test::run();
    maicord::test_exit(maicord::edited_label_test::failures, "edited_label");
}

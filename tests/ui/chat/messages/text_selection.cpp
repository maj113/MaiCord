// A text selection over the timeline spans messages, as a browser selection over the official client's message list:
// a drag from one message extends over the following or preceding ones, within a group and across groups, every
// message between is selected whole and the copy joins their text in order (author headers included as shown). The
// ends are message ids and offsets, so the selection survives scrolling and virtualization, and an end whose message
// is deleted or edited bounds the selection at that message.
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/chat/messages/text_selection.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/text/pointer_selection.hpp"

#include "../../../support/test_output.hpp"
#include "../../../support/ui_scene_fixtures.hpp"

namespace maicord::message_text_selection_test {
namespace {
u32 failures;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}

constexpr u64 kChannel = 7;
constexpr u64 kAnn = 9, kBob = 10;
constexpr u64 kStartMs = 1'700'000'000'000ull;
constexpr u64 kNow = 1'700'000'100ull;
constexpr u32 kRowCount = 12;
constexpr u32 kWidgets = kRowCount * kMessageSceneWidgetsPerRow + 1;
constexpr UiRect kTall{0, 0, 720, 2'000};
constexpr UiRect kShort{0, 0, 720, 200};

MessageRecord records[kRowCount];
u32 order[kRowCount];
u8 store_text[4096];
MessageRichRecord rich_records[kRowCount];
u16 rich_order[kRowCount];
u8 rich_text[8192];
MessageSceneRowWidget rows[kRowCount];
UiWidget widgets[kWidgets];
UiLayoutNode layout[kWidgets];
MessageTextCommand commands[256];
char display[8192];
TextStyleSpan styles[256];
MessageMarkdownSemantic semantics[128];
MessageTextMeasurement measurements[kRowCount * 2];
MessageTextSpoilerReveal reveals[16];
u32 scratch[8192];
UserRecord user_records[2];
u32 user_order[2];
u8 user_names[128];

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
    UserStore users{};
    UiRect viewport{};
    MessageTextSelectionRange range{};
    UiTextPointerSelection unit{};
    MessageTextSelectionText text{};
};
Fixture g_fixture;

struct Expected {
    char data[2048];
    u32 bytes;
    void add(Utf8View view) noexcept {
        for (u32 i = 0; i < view.bytes && bytes < sizeof(data); ++i)
            data[bytes++] = view.data[i];
    }
    Utf8View view() const noexcept {
        return {data, bytes};
    }
};

Utf8View message_content(u32 index) noexcept {
    switch (index) {
    case 0: return utf8_literal("first in group");
    case 1: return utf8_literal("second in group");
    case 2: return utf8_literal("third");
    case 3: return utf8_literal("other author");
    case 4: return utf8_literal("fifth");
    case 5: return utf8_literal("sixth");
    case 6: return utf8_literal("seventh");
    case 7: return utf8_literal("eighth");
    case 8: return utf8_literal("ninth");
    case 9: return utf8_literal("tenth");
    case 10: return utf8_literal("eleventh");
    default: return utf8_literal("last of all");
    }
}
constexpr u64 id_of(u32 index) noexcept {
    return 101 + index;
}
// 101-103 Ann, 104-106 Bob, 107-112 Ann: three groups. 103 was edited.
MessageInput input_of(u32 index) noexcept {
    return MessageInput{id_of(index),  kChannel, index >= 3 && index < 6 ? kBob : kAnn, kStartMs + index * 1'000ull,
                        index == 2 ? kStartMs + 60'000 : 0, message_content(index), 0};
}

bool rebuild(Fixture& f) noexcept {
    return message_timeline_set_viewport(&f.timeline, ui_extent_units(ui_rect_height(f.viewport))) &&
           message_scene_update(&f.scene, f.viewport).ok() &&
           message_text_presentation_build(&f.presentation, f.scene, f.state, &f.users, f.navigation, kDefaultTheme,
                                           kNow)
               .ok() &&
           // As the client does after every build of the timeline's text.
           message_text_selection_note(&f.text, f.range, f.presentation);
}
bool scroll_to(Fixture& f, u64 units) noexcept {
    return message_timeline_set_scroll(&f.timeline, units, false) && rebuild(f);
}
bool presented(const MessageTextPresentation& p, u64 id) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].message_id == id)
            return true;
    return false;
}
u32 content_index(const MessageTextPresentation& p, u64 id) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].message_id == id && p.commands[i].measurement_kind == MessageTextMeasurementKind::content)
            return i;
    return kMessageTextSelectionNoCommand;
}
// The author header of a group's first message: its first command, with the name and time.
u32 header_index(const MessageTextPresentation& p, u64 id) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].message_id == id)
            return p.commands[i].measurement_kind == MessageTextMeasurementKind::none ? i
                                                                                       : kMessageTextSelectionNoCommand;
    return kMessageTextSelectionNoCommand;
}
u32 first_selectable(const MessageTextPresentation& p) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (message_text_command_selectable(p.commands[i]))
            return i;
    return kMessageTextSelectionNoCommand;
}
// The selectable commands from..through as a copy shows them, a line each ("(edited)" left out).
void add_shown(Expected* expected, const MessageTextPresentation& p, u32 from, u32 through) noexcept {
    bool first = true;
    for (u32 i = from; i <= through && i < p.command_count; ++i) {
        if (!message_text_command_selectable(p.commands[i]))
            continue;
        if (!first)
            expected->add(utf8_literal("\n"));
        expected->add(Utf8View{p.commands[i].text.data, message_text_command_copy_end(p.commands[i])});
        first = false;
    }
}
Utf8View text_at(const MessageTextPresentation& p, u32 index) noexcept {
    return index < p.command_count ? p.commands[index].text : Utf8View{};
}
// A drag on to `to`/`to_offset` as the client makes it: a focus whose text does not fit is refused and the selection
// stays where it last fit. True when the selection now ends there.
bool extend(Fixture& f, u32 to, u32 to_offset) noexcept {
    const MessageTextSelectionRange previous = f.range;
    if (!message_text_selection_extend(&f.range, f.unit, f.presentation, f.store, to, to_offset))
        return false;
    if (message_text_selection_note(&f.text, f.range, f.presentation))
        return true;
    f.range = previous;
    message_text_selection_note(&f.text, f.range, f.presentation);
    return false;
}
// A press at `index`/`offset` and a drag to `to`/`to_offset`.
bool drag(Fixture& f, u32 index, u32 offset, u32 to, u32 to_offset, u8 clicks = 1) noexcept {
    message_text_selection_text_release(&f.text);
    reset_value(&f.range);
    if (index >= f.presentation.command_count || to >= f.presentation.command_count ||
        !message_text_selection_begin(&f.range, &f.unit, f.presentation, index, offset, clicks) ||
        !message_text_selection_note(&f.text, f.range, f.presentation))
        return false;
    extend(f, to, to_offset);
    return true;
}
bool g_overflow;
Utf8View copy(Fixture& f) noexcept {
    return message_text_selection_copy(&f.text, f.range, f.store, &f.rich, &f.scene, &g_overflow);
}
bool reset_timeline(Fixture& f) noexcept {
    message_text_selection_text_release(&f.text);
    reset_value(&f.range);
    reset_value(&f.store);
    reset_value(&f.rich);
    reset_value(&f.scene);
    reset_value(&f.list);
    reset_value(&f.timeline);
    reset_value(&f.presentation);
    bool ok = message_store_initialize(&f.store, {records, kRowCount}, {order, kRowCount},
                                       {store_text, sizeof(store_text)}) &&
              message_rich_store_initialize(&f.rich, {rich_records, kRowCount}, {rich_order, kRowCount},
                                            {rich_text, sizeof(rich_text)});
    for (u32 i = 0; ok && i < kRowCount; ++i)
        ok = message_store_upsert(&f.store, input_of(i)).ok();
    const u32 default_units = ui_extent_units(80.0f);
    f.viewport = kTall;
    return ok && ui_variable_list_initialize(&f.list, {f.units, kRowCount}, {f.fenwick, kRowCount}, kRowCount,
                                             default_units) &&
           message_timeline_initialize(&f.timeline, &f.store, &f.list, default_units,
                                       ui_extent_units(ui_rect_height(kTall)), false) &&
           message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
           message_scene_initialize(&f.scene, &f.timeline, {rows, kRowCount}, {widgets, kWidgets},
                                    {layout, kWidgets}) &&
           message_text_presentation_initialize(&f.presentation, {commands, 256}, {display, sizeof(display)},
                                                {styles, 256}, {semantics, 128}, {measurements, kRowCount * 2},
                                                {reveals, 16}, {scratch, 8192}) &&
           scroll_to(f, 0);
}

void test_within_and_across_groups(Fixture& f) noexcept {
    const MessageTextPresentation& p = f.presentation;
    const u32 first = content_index(p, id_of(0)), second = content_index(p, id_of(1));
    const u32 third = content_index(p, id_of(2)), other = content_index(p, id_of(3));
    const u32 bob = header_index(p, id_of(3));
    check(first != kMessageTextSelectionNoCommand && second != kMessageTextSelectionNoCommand &&
              third != kMessageTextSelectionNoCommand && other != kMessageTextSelectionNoCommand &&
              bob != kMessageTextSelectionNoCommand && header_index(p, id_of(0)) != kMessageTextSelectionNoCommand &&
              header_index(p, id_of(1)) == kMessageTextSelectionNoCommand,
          "selection: the fixture shows two groups, each led by its author header");

    // One group: from inside its first message into its second.
    check(drag(f, first, 6, second, 6) && utf8_equal(copy(f), utf8_literal("in group\nsecond")),
          "selection: a drag from one message into the next of its group copies both parts, joined by a newline");
    MessageTextSelectionSpan span = message_text_selection_span(f.range, p);
    check(span.valid && span.first == first && span.first_offset == 6 && span.last == second && span.last_offset == 6,
          "selection: the highlight runs from the anchor in one message to the focus in the next");

    // Across groups: the next group's author header is copied as shown, before its text; "(edited)" stays out.
    Expected across{};
    across.add(utf8_literal("third\n"));
    across.add(text_at(p, bob));
    across.add(utf8_literal("\nother author"));
    check(drag(f, third, 0, other, text_at(p, other).bytes) && utf8_equal(copy(f), across.view()),
          "selection: a drag across groups copies the next author's header line and text, without (edited)");
    check(drag(f, other, text_at(p, other).bytes, third, 0) && utf8_equal(copy(f), across.view()),
          "selection: a drag upwards copies the same text in timeline order");

    // Middle messages are selected whole: every command between the ends lies in the highlighted span.
    check(drag(f, first, 6, other, 5), "selection: a drag over four messages");
    span = message_text_selection_span(f.range, p);
    bool middle = span.valid && span.first == first && span.last == other && span.last_offset == 5;
    for (u32 i = second; middle && i <= third; ++i)
        middle = i > span.first && i < span.last;
    check(middle && bob > span.first && bob < span.last,
          "selection: the messages between the ends and the header between them are highlighted whole");
    Expected four{};
    four.add(utf8_literal("in group\nsecond in group\nthird\n"));
    four.add(text_at(p, bob));
    four.add(utf8_literal("\nother"));
    check(utf8_equal(copy(f), four.view()), "selection: the copy of four messages joins them in order");

    // Double and triple clicks still select a word or the paragraph of one message; a drag on keeps that unit.
    check(drag(f, second, 1, second, 1, 2) && f.range.anchor.byte_offset == 0 && f.range.focus.byte_offset == 6 &&
              utf8_equal(copy(f), utf8_literal("second")),
          "selection: a double click selects the word");
    check(extend(f, third, 2) && f.range.anchor.byte_offset == 0 &&
              utf8_equal(copy(f), utf8_literal("second in group\nth")),
          "selection: a word selection dragged into the next message keeps its whole first word");
    check(extend(f, first, 6) && f.range.anchor.byte_offset == 6 &&
              utf8_equal(copy(f), utf8_literal("in group\nsecond")),
          "selection: dragged back into the message before, it keeps the word from its end");
    check(drag(f, third, 1, third, 1, 3) && utf8_equal(copy(f), utf8_literal("third")),
          "selection: a triple click selects the paragraph of one message");

    // A click without a drag selects nothing and copies nothing; it keeps only its own message's text.
    message_text_selection_text_release(&f.text);
    check(message_text_selection_begin(&f.range, &f.unit, p, first, 3, 1) && !message_text_selection_nonempty(f.range) &&
              message_text_selection_note(&f.text, f.range, p) && f.text.count == 1 &&
              !message_text_selection_span(f.range, p).valid && copy(f).bytes == 0,
          "selection: a click alone is an empty selection");
}

void test_scroll_and_virtualization(Fixture& f) noexcept {
    // The whole timeline, presented at once, is the reference copy.
    const MessageTextPresentation& p = f.presentation;
    check(drag(f, content_index(p, id_of(0)), 6, content_index(p, id_of(kRowCount - 1)),
               text_at(p, content_index(p, id_of(kRowCount - 1))).bytes),
          "virtualization: a selection over the whole timeline");
    Expected reference{};
    reference.add(copy(f));
    const MessageTextSelectionRange whole = f.range;
    check(reference.bytes != 0 && test_contains(reference.view(), text_at(p, header_index(p, id_of(6)))) &&
              test_contains(reference.view(), utf8_literal("last of all")),
          "virtualization: the reference copy holds every group's header and the last message");

    // A short viewport: the drag starts at the top and autoscrolls to the bottom, a step at a time.
    f.viewport = kShort;
    message_text_selection_text_release(&f.text);
    reset_value(&f.range);
    check(scroll_to(f, 0) && !presented(p, id_of(kRowCount - 1)) &&
              message_text_selection_begin(&f.range, &f.unit, p, content_index(p, id_of(0)), 6, 1),
          "virtualization: a press on the first message of a short timeline that does not show the last");
    const u64 maximum_scroll = message_timeline_maximum_scroll(f.timeline);
    bool stepped = maximum_scroll != 0;
    for (u64 scroll = 0; stepped;) {
        scroll = minimum(scroll + ui_extent_units(60.0f), maximum_scroll);
        stepped = scroll_to(f, scroll);
        // The held drag sits at the bottom edge: the focus follows the last visible text.
        u32 last = kMessageTextSelectionNoCommand;
        for (u32 i = 0; stepped && i < p.command_count; ++i)
            if (message_text_command_selectable(p.commands[i]) &&
                p.commands[i].measurement_kind == MessageTextMeasurementKind::content &&
                p.commands[i].rectangle.top < f.viewport.bottom)
                last = i;
        stepped = stepped && last != kMessageTextSelectionNoCommand;
        if (stepped) {
            extend(f, last, text_at(p, last).bytes);
            stepped = !f.text.incomplete;
        }
        if (scroll == maximum_scroll)
            break;
    }
    check(stepped && !presented(p, id_of(0)) && presented(p, id_of(kRowCount - 1)) &&
              f.range.focus.message_id == id_of(kRowCount - 1),
          "virtualization: the drag reached the last message and the first left the presentation");
    MessageTextSelectionSpan span = message_text_selection_span(f.range, p);
    check(span.valid && span.first == first_selectable(p) && span.first_offset == 0 &&
              span.last == content_index(p, id_of(kRowCount - 1)) &&
              span.last_offset == text_at(p, span.last).bytes,
          "virtualization: with the anchor scrolled out, every presented message up to the focus stays highlighted");
    check(utf8_equal(copy(f), reference.view()),
          "virtualization: the copy holds the messages scrolled out of the presentation, as the reference");

    // Scrolling back brings the anchor's own offset back.
    check(scroll_to(f, 0) && presented(p, id_of(0)), "virtualization: scrolled back to the top");
    span = message_text_selection_span(f.range, p);
    check(span.valid && span.first == content_index(p, id_of(0)) && span.first_offset == 6 &&
              span.last == p.command_count - 1 && utf8_equal(copy(f), reference.view()),
          "virtualization: the selection survives the scroll back with its anchor where it was");
    check(f.range.anchor.message_id == whole.anchor.message_id &&
              f.range.anchor.byte_offset == whole.anchor.byte_offset,
          "virtualization: the selection is held as a message id and offset, not a row");

    // Rebuilds of unchanged text keep nothing new: the kept text does not grow per build or per scroll.
    // (The rebuild first drops the last copy's output.)
    const bool settled = rebuild(f);
    const usize kept = f.text.data.offset;
    const u32 count = f.text.count;
    check(settled && rebuild(f) && rebuild(f) && scroll_to(f, ui_extent_units(40.0f)) && scroll_to(f, 0) &&
              f.text.data.offset == kept && f.text.count == count,
          "virtualization: rebuilds and scrolls of unchanged messages record nothing again");

    // A jump: messages scrolled past without being presented copy their stored text.
    message_text_selection_text_release(&f.text);
    reset_value(&f.range);
    check(scroll_to(f, 0) &&
              message_text_selection_begin(&f.range, &f.unit, p, content_index(p, id_of(0)), 0, 1) &&
              message_text_selection_note(&f.text, f.range, p) && scroll_to(f, maximum_scroll),
          "virtualization: a press at the top, then a jump to the bottom");
    const u32 last = content_index(p, id_of(kRowCount - 1));
    check(!presented(p, id_of(4)) && last != kMessageTextSelectionNoCommand &&
              extend(f, last, text_at(p, last).bytes),
          "virtualization: the drag lands on the last message past rows never presented");
    const Utf8View jumped = copy(f);
    check(test_contains(jumped, utf8_literal("first in group\nsecond in group")) &&
              test_contains(jumped, utf8_literal("fifth\nsixth")) &&
              test_contains(jumped, utf8_literal("last of all")),
          "virtualization: messages never presented while selected copy their stored text, in order");
    f.viewport = kTall;
    check(scroll_to(f, 0), "virtualization: the whole timeline again");
}

void test_deleted_and_edited_ends(Fixture& f) noexcept {
    const MessageTextPresentation& p = f.presentation;
    check(drag(f, content_index(p, id_of(0)), 6, content_index(p, id_of(3)), 5),
          "ends: a selection from the first message into the fourth");
    // The anchor's message is deleted: the selection keeps what lies past it.
    check(message_timeline_remove_local(&f.timeline, id_of(0)).ok() && rebuild(f) && !presented(p, id_of(0)),
          "ends: the anchor's message is deleted");
    MessageTextSelectionSpan span = message_text_selection_span(f.range, p);
    check(span.valid && span.first == first_selectable(p) && span.first_offset == 0 &&
              p.commands[span.first].message_id == id_of(1) && span.last == content_index(p, id_of(3)) &&
              span.last_offset == 5,
          "ends: the highlight starts at the message after the deleted anchor");
    Expected after_delete{};
    add_shown(&after_delete, p, first_selectable(p), content_index(p, id_of(2)));
    after_delete.add(utf8_literal("\n"));
    add_shown(&after_delete, p, header_index(p, id_of(3)), header_index(p, id_of(3)));
    after_delete.add(utf8_literal("\nother"));
    check(header_index(p, id_of(1)) == first_selectable(p) && utf8_equal(copy(f), after_delete.view()),
          "ends: the copy starts with the message after the deleted anchor, its new header included");

    // The focus's message is edited: the selection ends before it.
    MessageInput edited = input_of(3);
    edited.content = utf8_literal("other author, edited");
    edited.edited_timestamp_ms = kStartMs + 90'000;
    check(message_store_upsert(&f.store, edited).ok() && rebuild(f), "ends: the focus's message is edited");
    span = message_text_selection_span(f.range, p);
    const u32 third = content_index(p, id_of(2));
    check(span.valid && span.last == third && span.last_offset == text_at(p, third).bytes,
          "ends: the highlight stops at the end of the message before the edited focus");
    Expected before_edit{};
    add_shown(&before_edit, p, first_selectable(p), third);
    check(utf8_equal(copy(f), before_edit.view()), "ends: the copy stops before the edited message");

    // Both ends in one message that is deleted: nothing is left.
    const u32 fifth = content_index(p, id_of(4));
    check(drag(f, fifth, 0, fifth, 3) && utf8_equal(copy(f), utf8_literal("fif")) &&
              message_timeline_remove_local(&f.timeline, id_of(4)).ok() && rebuild(f) &&
              !message_text_selection_span(f.range, p).valid && copy(f).bytes == 0,
          "ends: a selection inside a deleted message shows and copies nothing");
}

void test_autoscroll() noexcept {
    const UiRect viewport{0, 100, 720, 500};
    const float inside = message_text_selection_autoscroll(viewport, 300, 16);
    const float above = message_text_selection_autoscroll(viewport, 90, 16);
    const float far_above = message_text_selection_autoscroll(viewport, -1'000, 16);
    const float below = message_text_selection_autoscroll(viewport, 560, 16);
    const float stalled = message_text_selection_autoscroll(viewport, -1'000, 5'000);
    check(inside == 0 && above < 0 && far_above < above && below > 0 &&
              message_text_selection_autoscroll(viewport, 90, 0) == 0,
          "autoscroll: a drag past the top scrolls up, farther faster, past the bottom down, inside not at all");
    check(far_above == message_text_selection_autoscroll(viewport, -5'000, 16) &&
              stalled == message_text_selection_autoscroll(viewport, -1'000, 100),
          "autoscroll: the speed and a stalled frame's step are capped");
}

// The kept text is bounded: a drag whose text would not fit selects no further, and what is highlighted is what the
// copy holds. Text that stops fitting later (an edit) makes the copy refuse rather than copy less.
void test_text_bound(Fixture& f) noexcept {
    check(reset_timeline(f), "bounds: a fresh timeline");
    const MessageTextPresentation& p = f.presentation;
    f.text.capacity = 1'024;
    const u32 first = content_index(p, id_of(0));
    check(drag(f, first, 0, first, 0), "bounds: a press on the first message with 1 KB for the selection's text");
    u32 reached = 0;
    bool refused = false;
    for (u32 i = 1; i < kRowCount && !refused; ++i) {
        const u32 content = content_index(p, id_of(i));
        if (extend(f, content, text_at(p, content).bytes))
            reached = i;
        else
            refused = true;
    }
    const u32 last = content_index(p, id_of(reached));
    check(refused && reached >= 2 && f.range.focus.message_id == id_of(reached) && !f.text.incomplete &&
              f.text.data.offset <= 512,
          "bounds: the drag stops at the last message whose text fits in half the capacity");
    const u32 beyond = content_index(p, id_of(kRowCount - 1));
    check(!extend(f, beyond, text_at(p, beyond).bytes) && f.range.focus.message_id == id_of(reached),
          "bounds: a drag farther on still selects no further");
    const MessageTextSelectionSpan span = message_text_selection_span(f.range, p);
    Expected shown{};
    add_shown(&shown, p, first, last);
    check(span.valid && span.first == first && span.first_offset == 0 && span.last == last &&
              span.last_offset == text_at(p, last).bytes && utf8_equal(copy(f), shown.view()) && !g_overflow,
          "bounds: the copy holds exactly the highlighted messages");

    // A selected message grows past what fits: the copy refuses and says so instead of copying less.
    static char grown[700];
    for (u32 i = 0; i < sizeof(grown); ++i)
        grown[i] = static_cast<char>('a' + i % 26);
    MessageInput edited = input_of(1);
    edited.content = Utf8View{grown, sizeof(grown)};
    // The rebuild's note reports the text that did not fit.
    check(message_store_upsert(&f.store, edited).ok() && !rebuild(f) && f.text.incomplete && copy(f).bytes == 0 &&
              g_overflow,
          "bounds: a selected message that no longer fits makes the copy refuse, reporting the overflow");
    message_text_selection_text_release(&f.text);
    f.text.capacity = 0;
}

// A selection spans at most kMessageTextSelectionMaximumMessages messages: the drag past them is refused.
constexpr u32 kMany = kMessageTextSelectionMaximumMessages + 2;
struct ManyFixture {
    MessageRecord records[kMany];
    u32 order[kMany];
    u8 text[kMany * 8];
    char content[kMany][8];
    u32 units[kMany];
    u64 fenwick[kMany];
    MessageStore store;
    MessageScene scene;
    UiVariableList list;
    MessageTimeline timeline;
    MessageTextPresentation presentation;
    ChatStateStore state;
    NavigationView navigation;
    MessageTextSelectionRange range;
    UiTextPointerSelection unit;
    MessageTextSelectionText kept;
};
ManyFixture g_many;

u32 many_content(const ManyFixture& m, u32 index) noexcept {
    return content_index(m.presentation, 1'000 + index);
}
bool many_build(ManyFixture& m, u32 top) noexcept {
    return message_timeline_set_scroll(&m.timeline, static_cast<u64>(top) * ui_extent_units(80.0f), false) &&
           message_scene_update(&m.scene, kShort).ok() &&
           message_text_presentation_build(&m.presentation, m.scene, m.state, nullptr, m.navigation, kDefaultTheme,
                                           kNow)
               .ok() &&
           message_text_selection_note(&m.kept, m.range, m.presentation);
}

void test_message_cap() noexcept {
    ManyFixture& m = g_many;
    const u32 units = ui_extent_units(80.0f);
    bool ready = message_store_initialize(&m.store, {m.records, kMany}, {m.order, kMany}, {m.text, sizeof(m.text)});
    for (u32 i = 0; ready && i < kMany; ++i) {
        char* out = m.content[i];
        u32 bytes = 0;
        out[bytes++] = 'm';
        char digits[8];
        u32 count = 0;
        for (u32 value = i; count == 0 || value != 0; value /= 10)
            digits[count++] = static_cast<char>('0' + value % 10);
        while (count)
            out[bytes++] = digits[--count];
        ready = message_store_upsert(&m.store, {1'000 + i, kChannel, kAnn, kStartMs + i * 1'000ull, 0,
                                                Utf8View{out, bytes}, 0})
                    .ok();
    }
    check(ready, "message cap: the stores hold more messages than the cap");
    // Plain text messages: no rich store (it would need room for every message).
    ready = ready && ui_variable_list_initialize(&m.list, {m.units, kMany}, {m.fenwick, kMany}, kMany, units) &&
            message_timeline_initialize(&m.timeline, &m.store, &m.list, units, ui_extent_units(ui_rect_height(kShort)),
                                        false);
    check(ready, "message cap: the timeline lays them out");
    ready = ready &&
            message_scene_initialize(&m.scene, &m.timeline, {rows, kRowCount}, {widgets, kWidgets},
                                     {layout, kWidgets}) &&
            message_text_presentation_initialize(&m.presentation, {commands, 256}, {display, sizeof(display)},
                                                 {styles, 256}, {semantics, 128}, {measurements, kRowCount * 2},
                                                 {reveals, 16}, {scratch, 8192});
    check(ready, "message cap: the scene and presentation initialize");
    ready = ready && many_build(m, 0);
    const u32 first = ready ? many_content(m, 0) : kMessageTextSelectionNoCommand;
    check(ready && first != kMessageTextSelectionNoCommand &&
              message_text_selection_begin(&m.range, &m.unit, m.presentation, first, 0, 1) &&
              message_text_selection_note(&m.kept, m.range, m.presentation),
          "message cap: a press on the first of more messages than a selection may span");
    if (!ready)
        return;
    // Scrolled (a long autoscroll) to the last message the cap allows and the one past it.
    constexpr u32 kLast = kMessageTextSelectionMaximumMessages - 1;
    const bool scrolled = many_build(m, kLast);
    const u32 last = scrolled ? many_content(m, kLast) : kMessageTextSelectionNoCommand;
    const u32 past = scrolled ? many_content(m, kLast + 1) : kMessageTextSelectionNoCommand;
    check(last != kMessageTextSelectionNoCommand && past != kMessageTextSelectionNoCommand &&
              message_text_selection_extend(&m.range, m.unit, m.presentation, m.store, last,
                                            text_at(m.presentation, last).bytes) &&
              message_text_selection_note(&m.kept, m.range, m.presentation),
          "message cap: a drag to the last message the cap allows selects it");
    const MessageTextSelectionRange at_cap = m.range;
    check(past != kMessageTextSelectionNoCommand &&
              !message_text_selection_extend(&m.range, m.unit, m.presentation, m.store, past, 1) &&
              m.range.signature == at_cap.signature && m.range.focus.message_id == 1'000 + kLast,
          "message cap: a drag one message farther selects no further");
    bool overflow = true;
    const Utf8View copied = message_text_selection_copy(&m.kept, m.range, m.store, nullptr, &m.scene, &overflow);
    u32 lines = copied.bytes ? 1 : 0;
    for (u32 i = 0; i < copied.bytes; ++i)
        lines += copied.data[i] == '\n' ? 1u : 0u;
    const Utf8View tail = text_at(m.presentation, last);
    check(!overflow && lines == kMessageTextSelectionMaximumMessages && copied.bytes > tail.bytes &&
              utf8_equal(Utf8View{copied.data, 3}, utf8_literal("m0\n")) &&
              utf8_equal(Utf8View{copied.data + copied.bytes - tail.bytes - 1, tail.bytes + 1},
                         utf8_literal("\nm9999")) &&
              m.kept.count <= 2 * kRowCount,
          "message cap: the copy holds exactly the capped messages, keeping only the presented ones' text");
    message_text_selection_text_release(&m.kept);
}

void run() noexcept {
    Fixture& f = g_fixture;
    const bool ready = user_store_initialize(&f.users, {user_records, 2}, {user_order, 2},
                                             {user_names, sizeof(user_names)}) &&
                       user_store_upsert(&f.users, kAnn, utf8_literal("Ann")).ok() &&
                       user_store_upsert(&f.users, kBob, utf8_literal("Bob")).ok() && reset_timeline(f);
    check(ready, "selection: fixture stores, scene and text presentation build");
    if (!ready)
        return;
    test_within_and_across_groups(f);
    test_scroll_and_virtualization(f);
    test_deleted_and_edited_ends(f);
    test_text_bound(f);
    test_autoscroll();
    test_message_cap();
    message_text_selection_text_release(&f.text);
}
} // namespace
} // namespace maicord::message_text_selection_test

MAICORD_TEST_ENTRY(maicord_message_text_selection_entry) {
    maicord::message_text_selection_test::run();
    maicord::test_exit(maicord::message_text_selection_test::failures, "message_text_selection");
}

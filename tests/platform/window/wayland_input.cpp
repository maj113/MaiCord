#include "maicord/core/shared_library.hpp"
#include "maicord/platform/window/wayland/click.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/window/wayland/keymap.hpp"
#include "maicord/platform/window/wayland/post_queue.hpp"
#include "maicord/platform/window/wayland/repeat.hpp"
#include "maicord/platform/window/wayland/uri.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"
#include "maicord/platform/window/wayland/settings_map.hpp"
#include "wayland_protocols.hpp"
#include "../../support/test_output.hpp"

// The pure parts of the Wayland backend (src/platform/window/wayland): the key table, the character filter, the
// double-click synthesiser and key repeat timing with an injected clock, the generated protocol tables against
// libwayland-client's own, and the key table over real xkb keymaps (US, German, French, Dvorak, Russian, Japanese).
// The last two need libwayland-client and libxkbcommon with xkeyboard-config; without them they report SKIP.

namespace maicord {
namespace {

using namespace wayland;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* left, const char* right) noexcept {
    if (!left || !right)
        return left == right;
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// The key table ------------------------------------------------------------------------------------------------------

void check_key(u32 evdev, u32 active, u32 level0, u32 level1, Key expected, const char* label) noexcept {
    check(key_from_keysyms(evdev, active, level0, level1) == expected, label);
}

void test_key_table() noexcept {
    // Position codes: the US layout of a 105-key board.
    check(key_from_position(30) == 'A' && key_from_position(44) == 'Z' && key_from_position(16) == 'Q', "position letters");
    check(key_from_position(2) == '1' && key_from_position(11) == '0', "position digits");
    check(key_from_position(12) == key::minus && key_from_position(13) == key::equals &&
              key_from_position(26) == key::left_bracket && key_from_position(27) == key::right_bracket &&
              key_from_position(39) == key::semicolon && key_from_position(40) == key::quote &&
              key_from_position(41) == key::grave && key_from_position(43) == key::backslash &&
              key_from_position(51) == key::comma && key_from_position(52) == key::period &&
              key_from_position(53) == key::slash,
          "position punctuation");
    check(key_from_position(59) == key::f1 && key_from_position(68) == key::f10 && key_from_position(87) == key::f11 &&
              key_from_position(88) == key::f12 && key_from_position(183) == key::f13 &&
              key_from_position(194) == key::f24,
          "position function keys");
    check(key_from_position(103) == key::up && key_from_position(108) == key::down &&
              key_from_position(105) == key::left && key_from_position(106) == key::right &&
              key_from_position(104) == key::page_up && key_from_position(109) == key::page_down,
          "position navigation");
    check(key_from_position(86) == 0xE2 && key_from_position(0) == key::none && key_from_position(500) == key::none,
          "position extra and unknown keys");

    // US: the VK is the label.
    check_key(30, 'a', 'a', 'A', 'A', "us a");
    check_key(30, 'A', 'a', 'A', 'A', "us shifted a");
    check_key(2, '1', '1', '!', '1', "us 1");
    check_key(12, keysym::minus, keysym::minus, '_', key::minus, "us minus");
    check_key(13, keysym::equal, keysym::equal, '+', key::equals, "us equals");
    check_key(41, keysym::grave, keysym::grave, '~', key::grave, "us grave");
    check_key(57, keysym::space, keysym::space, keysym::space, key::space, "space");

    // German: Z and Y trade places, the umlaut keys keep their position.
    check_key(21, 'z', 'z', 'Z', 'Z', "de y position types z");
    check_key(44, 'y', 'y', 'Y', 'Y', "de z position types y");
    check_key(26, 0xFC, 0xFC, 0xDC, key::left_bracket, "de u-umlaut by position");
    check_key(39, 0xF6, 0xF6, 0xD6, key::semicolon, "de o-umlaut by position");
    check_key(12, 0xDF, 0xDF, '?', key::minus, "de sharp s by position");

    // French AZERTY: the digit row types symbols unshifted; the digit is the shifted level.
    check_key(2, '&', '&', '1', '1', "fr digit 1 from level 1");
    check_key(3, 0xE9, 0xE9, '2', '2', "fr digit 2 from level 1");
    check_key(16, 'a', 'a', 'A', 'A', "fr q position types a");
    check_key(30, 'q', 'q', 'Q', 'Q', "fr a position types q");

    // Dvorak: the VK follows the label, punctuation included.
    check_key(31, 'o', 'o', 'O', 'O', "dvorak s position types o");
    check_key(12, keysym::bracketleft, keysym::bracketleft, '{', key::left_bracket, "dvorak minus position types [");
    check_key(16, keysym::apostrophe, keysym::apostrophe, '"', key::quote, "dvorak q position types '");

    // Russian: letters fall back to the physical key; the digit row and the shared punctuation are Latin.
    check_key(30, 0x6C6, 0x6C6, 0x6E6, 'A', "ru ef on the a key");
    check_key(44, 0x6D1, 0x6D1, 0x6F1, 'Z', "ru ya on the z key");
    check_key(51, 0x6C2, 0x6C2, 0x6E2, key::comma, "ru be on the comma key");
    check_key(2, '1', '1', '!', '1', "ru digit 1");

    // Japanese: yen and ro keep their positions; the kana have no key of their own.
    check_key(124, 0xA5, 0xA5, '|', key::backslash, "jp yen key");
    check_key(89, 0xFF2C, 0xFF2C, 0xFF2C, 0xE2, "jp ro key by position");

    // Fixed keysyms win over the layout: keypad, modifiers, navigation.
    check_key(79, 0xFFB1, 0xFF9C, 0xFFB1, key::numpad1, "keypad 1 with Num Lock");
    check_key(79, keysym::kp_end, keysym::kp_end, 0xFFB1, key::end, "keypad 1 without Num Lock");
    check_key(83, keysym::kp_delete, keysym::kp_delete, keysym::kp_decimal, key::del, "keypad dot without Num Lock");
    check_key(83, keysym::kp_decimal, keysym::kp_delete, keysym::kp_decimal, key::decimal, "keypad dot with Num Lock");
    check_key(96, keysym::kp_enter, keysym::kp_enter, keysym::kp_enter, key::enter, "keypad enter");
    check_key(98, keysym::kp_divide, keysym::kp_divide, keysym::kp_divide, key::divide, "keypad divide");
    check_key(42, keysym::shift_l, keysym::shift_l, keysym::shift_l, key::shift, "left shift is the generic shift");
    check_key(54, keysym::shift_r, keysym::shift_r, keysym::shift_r, key::shift, "right shift is the generic shift");
    check_key(29, keysym::control_l, keysym::control_l, keysym::control_l, key::ctrl, "ctrl");
    check_key(56, keysym::alt_l, keysym::alt_l, keysym::alt_l, key::alt, "alt");
    check_key(100, keysym::iso_level3_shift, keysym::iso_level3_shift, keysym::iso_level3_shift, key::right_alt,
              "AltGr is right alt");
    check_key(125, keysym::super_l, keysym::super_l, keysym::super_l, key::left_super, "left super");
    check_key(127, keysym::menu, keysym::menu, keysym::menu, key::menu, "menu");
    check_key(15, keysym::iso_left_tab, keysym::tab, keysym::iso_left_tab, key::tab, "shift tab is tab");
    check_key(28, keysym::return_key, keysym::return_key, keysym::return_key, key::enter, "return");
    check_key(14, keysym::backspace, keysym::backspace, keysym::backspace, key::backspace, "backspace");
    check_key(111, keysym::delete_key, keysym::delete_key, keysym::delete_key, key::del, "delete");
    check_key(58, keysym::caps_lock, keysym::caps_lock, keysym::caps_lock, key::caps_lock, "caps lock");
    check_key(69, keysym::num_lock, keysym::num_lock, keysym::num_lock, key::num_lock, "num lock");
    check_key(59, keysym::f1, keysym::f1, keysym::f1, key::f1, "f1");
    check_key(194, keysym::f24, keysym::f24, keysym::f24, key::f24, "f24");
    check_key(99, keysym::print, keysym::print, keysym::sys_req, key::print_screen, "print");
    check_key(119, keysym::pause, keysym::pause, keysym::pause, key::pause, "pause");
    check_key(1, keysym::escape, keysym::escape, keysym::escape, key::escape, "escape");

    check(specific_modifier_key(42) == key::left_shift && specific_modifier_key(54) == key::right_shift &&
              specific_modifier_key(29) == key::left_ctrl && specific_modifier_key(97) == key::right_ctrl &&
              specific_modifier_key(56) == key::left_alt && specific_modifier_key(100) == key::right_alt &&
              specific_modifier_key(30) == key::none,
          "side-specific modifiers");
}

// Characters ---------------------------------------------------------------------------------------------------------

void test_characters() noexcept {
    check(is_message_character('a') && is_message_character(' ') && is_message_character('~'), "printable ascii");
    check(is_message_character('\b') && is_message_character('\t') && is_message_character('\r') &&
              is_message_character('\n') && is_message_character(0x1B),
          "editing controls");
    check(is_message_character(0x01) && is_message_character(0x1A), "ctrl+letter codes");
    check(!is_message_character(0x7F), "delete types no text");
    check(!is_message_character(0) && !is_message_character(0x80) && !is_message_character(0x9F), "nul and C1 controls");
    check(is_message_character(0xA0) && is_message_character(0xE9) && is_message_character(0x20AC) &&
              is_message_character(0x1F600) && is_message_character(0x10FFFF),
          "non-ascii text");
    check(!is_message_character(0xD800) && !is_message_character(0xDFFF) && !is_message_character(0x110000),
          "surrogates and beyond unicode");

    u16 units[2] = {0, 0};
    check(utf16_units('a', units) == 1 && units[0] == 'a', "utf16 ascii");
    check(utf16_units(0x20AC, units) == 1 && units[0] == 0x20AC, "utf16 bmp");
    check(utf16_units(0x1F600, units) == 2 && units[0] == 0xD83D && units[1] == 0xDE00, "utf16 pair");
    check(utf16_units(0x10000, units) == 2 && units[0] == 0xD800 && units[1] == 0xDC00, "utf16 first pair");
    check(utf16_units(0x10FFFF, units) == 2 && units[0] == 0xDBFF && units[1] == 0xDFFF, "utf16 last pair");
    check(utf16_units(0xD800, units) == 0 && utf16_units(0x110000, units) == 0, "utf16 rejects non-scalars");
}

// Double click -------------------------------------------------------------------------------------------------------

void test_double_click() noexcept {
    const ClickPolicy policy{500, 4.0f, 4.0f};
    ClickTracker tracker{};
    check(!click_tracker_press(&tracker, 0, 1000, 50, 50, policy), "first press is single");
    check(click_tracker_press(&tracker, 0, 1400, 51, 49, policy), "second press within time and rectangle");
    check(!click_tracker_press(&tracker, 0, 1450, 51, 49, policy), "after a double click the sequence starts again");
    check(click_tracker_press(&tracker, 0, 1900, 50, 50, policy), "the press that follows a plain one can double");

    click_tracker_reset(&tracker);
    check(!click_tracker_press(&tracker, 0, 0, 10, 10, policy), "press after reset");
    check(!click_tracker_press(&tracker, 1, 100, 10, 10, policy), "another button is no double click");
    check(!click_tracker_press(&tracker, 0, 200, 10, 10, policy), "the other button replaced the first press");

    check(!click_tracker_press(&tracker, 0, 10000, 10, 10, policy), "restart");
    check(!click_tracker_press(&tracker, 0, 10501, 10, 10, policy), "one millisecond too late");
    check(click_tracker_press(&tracker, 0, 11001, 10, 10, policy), "exactly the interval is in time");

    check(!click_tracker_press(&tracker, 0, 20000, 10, 10, policy), "restart far");
    check(!click_tracker_press(&tracker, 0, 20100, 13, 10, policy), "1.5 px either side is the rectangle: 3 px is out");
    check(click_tracker_press(&tracker, 0, 20200, 14, 10, policy), "within the rectangle of the second press");

    check(!click_tracker_press(&tracker, 0, 30000, 10, 10, policy), "restart tall");
    check(!click_tracker_press(&tracker, 0, 30100, 10, 12.5f, policy), "vertical distance outside");

    // The millisecond clock wraps.
    check(!click_tracker_press(&tracker, 2, 0xFFFFFFF0u, 5, 5, policy), "wrap first");
    check(click_tracker_press(&tracker, 2, 0x10u, 5, 5, policy), "wrap second within the interval");
}

// Key repeat ---------------------------------------------------------------------------------------------------------

void test_key_repeat() noexcept {
    KeyRepeat repeat{};
    check(key_repeat_wait_ms(repeat, 0) == kRepeatNever && !key_repeat_fire(&repeat, 0), "idle repeat");

    key_repeat_start(&repeat, 30, 1000, 500, 25);  // 25 per second: every 40 ms after 500 ms
    check(key_repeat_wait_ms(repeat, 1000) == 500, "waits the delay");
    check(!key_repeat_fire(&repeat, 1499), "not due before the delay");
    check(key_repeat_wait_ms(repeat, 1499) == 1, "one millisecond left");
    check(key_repeat_fire(&repeat, 1500), "first repeat at the delay");
    check(!key_repeat_fire(&repeat, 1500), "one repeat per call");
    check(key_repeat_wait_ms(repeat, 1500) == 40, "the interval follows");
    check(!key_repeat_fire(&repeat, 1539) && key_repeat_fire(&repeat, 1540), "second repeat on the interval");

    // A late loop gets one repeat and the cadence stays on the 40 ms grid after the delay.
    check(key_repeat_fire(&repeat, 1700), "late repeat fires once");
    check(key_repeat_wait_ms(repeat, 1700) == 40 && !key_repeat_fire(&repeat, 1739) && key_repeat_fire(&repeat, 1740),
          "the cadence is kept after a late repeat");

    key_repeat_release(&repeat, 31);
    check(repeat.armed, "another key's release keeps repeating");
    key_repeat_release(&repeat, 30);
    check(!repeat.armed && key_repeat_wait_ms(repeat, 5000) == kRepeatNever, "the repeating key's release stops it");

    key_repeat_start(&repeat, 30, 0, 100, 0);
    check(!repeat.armed, "a rate of 0 disables repeat");
    key_repeat_start(&repeat, 30, 0, 100, 10);
    key_repeat_start(&repeat, 44, 50, 100, 10);
    check(repeat.token == 44 && key_repeat_wait_ms(repeat, 50) == 100, "a new key replaces the repeating one");
    key_repeat_stop(&repeat);
    check(!key_repeat_fire(&repeat, 100000), "stopped");

    key_repeat_start(&repeat, 1, 0, 0, 5000);
    check(key_repeat_fire(&repeat, 0) && key_repeat_wait_ms(repeat, 0) == 1, "a rate above 1000 per second repeats every millisecond");
}

// File URIs and uri-lists ------------------------------------------------------------------------------------------------

bool decodes_to(const char* text, const char* expected) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    char output[64];
    u32 written = 0;
    if (!uri_percent_decode(text, length, output, sizeof(output) - 1, &written))
        return false;
    output[written] = 0;
    return text_equal(output, expected);
}

bool path_of(const char* uri, const char* expected) noexcept {
    u32 length = 0;
    while (uri[length])
        ++length;
    char output[64];
    u32 written = 0;
    if (!uri_file_path(uri, length, output, sizeof(output) - 1, &written))
        return false;
    output[written] = 0;
    return text_equal(output, expected);
}

bool rejected(const char* uri) noexcept {
    u32 length = 0;
    while (uri[length])
        ++length;
    char output[64];
    u32 written = 0;
    return !uri_file_path(uri, length, output, sizeof(output), &written);
}

bool wide_equals(const DroppedFiles* files, u32 index, const wchar_t* expected) noexcept {
    wchar_t buffer[128];
    const u32 length = dropped_files_path(files, index, Slice<wchar_t>{buffer, 128});
    u32 expected_length = 0;
    while (expected[expected_length])
        ++expected_length;
    if (length != expected_length)
        return false;
    for (u32 i = 0; i < length; ++i)
        if (buffer[i] != expected[i])
            return false;
    return true;
}

void test_uri() noexcept {
    check(decodes_to("plain", "plain") && decodes_to("a%20b", "a b") && decodes_to("%C3%A9", "\xC3\xA9") && decodes_to("%2f%2F", "//"),
          "percent decoding");
    char output[8];
    u32 written = 0;
    check(!uri_percent_decode("%zz", 3, output, sizeof(output), &written) && !uri_percent_decode("%4", 2, output, sizeof(output), &written) &&
              !uri_percent_decode("a%", 2, output, sizeof(output), &written) && !uri_percent_decode("%00", 3, output, sizeof(output), &written),
          "malformed and NUL escapes are refused");
    check(!uri_percent_decode("0123456789", 10, output, sizeof(output), &written), "a result that does not fit is refused");

    check(path_of("file:///tmp/a%20b.txt", "/tmp/a b.txt") && path_of("file://localhost/x/y", "/x/y") && path_of("file:///", "/"),
          "file URIs become local paths");
    check(rejected("http://example.org/x") && rejected("file://otherhost/x") && rejected("file://") && rejected("/tmp/plain") &&
              rejected("file://localhost") && rejected("file:///bad%zz"),
          "other schemes, hosts and malformed URIs are refused");

    const char list[] = "# a comment\r\nfile:///tmp/a%20b.txt\r\n\r\nfile://otherhost/skipped\nfile:///tmp/c%C3%A9.png\nhttp://nope\n";
    DroppedFiles* files = uri_list_files(reinterpret_cast<const u8*>(list), sizeof(list) - 1);
    check(files && dropped_files_count(files) == 2 && wide_equals(files, 0, L"/tmp/a b.txt") &&
              wide_equals(files, 1, L"/tmp/c\u00E9.png"),
          "a uri-list gives its local files, comments, blank lines and remote hosts skipped");
    wchar_t small[4];
    check(dropped_files_path(files, 0, Slice<wchar_t>{small, 4}) == 0 && dropped_files_path(files, 2, Slice<wchar_t>{small, 4}) == 0,
          "a path that does not fit, or an index past the end, gives nothing");
    dropped_files_release(files);

    const char gnome[] = "copy\nfile:///tmp/g.txt\nfile:///tmp/h.txt";
    files = uri_list_files(reinterpret_cast<const u8*>(gnome), sizeof(gnome) - 1);
    check(files && dropped_files_count(files) == 2 && wide_equals(files, 0, L"/tmp/g.txt") && wide_equals(files, 1, L"/tmp/h.txt"),
          "GNOME's list skips its leading copy line");
    dropped_files_release(files);
    const char cut[] = "cut\r\nfile:///x";
    files = uri_list_files(reinterpret_cast<const u8*>(cut), sizeof(cut) - 1);
    check(files && dropped_files_count(files) == 1, "and its cut line");
    dropped_files_release(files);

    const char none[] = "# nothing\nhttp://a\n";
    check(uri_list_files(reinterpret_cast<const u8*>(none), sizeof(none) - 1) == nullptr && uri_list_files(nullptr, 0) == nullptr,
          "a list without a local file gives none");

    // More lines than a drop holds: the first kMaxDroppedFiles.
    static char many[400 * 16];
    u32 used = 0;
    for (u32 i = 0; i < 300; ++i) {
        const char line[] = "file:///f";
        for (const char c : line)
            if (c)
                many[used++] = c;
        many[used++] = static_cast<char>('0' + i % 10);
        many[used++] = '\n';
    }
    files = uri_list_files(reinterpret_cast<const u8*>(many), used);
    check(files && dropped_files_count(files) == kMaxDroppedFiles, "a drop holds at most kMaxDroppedFiles files");
    dropped_files_release(files);
}

// The queue behind window_post --------------------------------------------------------------------------------------------

struct PostProducer {
    PostQueue* queue;
    u32 id;
    u32 count;
};

u32 produce(void* argument) noexcept {
    const PostProducer& producer = *static_cast<const PostProducer*>(argument);
    for (u32 i = 0; i < producer.count; ++i) {
        const PostEntry entry{producer.id, i, ~static_cast<u64>(i)};
        while (!post_queue_push(producer.queue, entry))
            thread_sleep(0);
    }
    return 0;
}

void test_post_queue() noexcept {
    static PostQueue queue;
    post_queue_init(&queue);
    PostEntry entry{};
    check(!post_queue_pop(&queue, &entry), "an empty queue pops nothing");
    for (u32 i = 0; i < kPostCapacity; ++i)
        check(post_queue_push(&queue, PostEntry{1, i, i * 2}), "a push into a queue with room");
    check(!post_queue_push(&queue, PostEntry{1, 0, 0}), "a full queue refuses the post");
    bool ordered = true;
    for (u32 i = 0; i < kPostCapacity; ++i)
        ordered = ordered && post_queue_pop(&queue, &entry) && entry.cause == 1 && entry.a == i && entry.b == i * 2;
    check(ordered && !post_queue_pop(&queue, &entry), "entries pop in the order they were pushed, then none");
    // The positions wrap: push and pop many more than the capacity.
    ordered = true;
    for (u32 i = 0; i < kPostCapacity * 5 + 7; ++i)
        ordered = ordered && post_queue_push(&queue, PostEntry{2, i, 0}) && post_queue_pop(&queue, &entry) && entry.a == i;
    check(ordered, "the queue keeps working past its capacity");

    // Four threads post while this one drains: nothing is lost, and each thread's posts keep their order.
    constexpr u32 kThreads = 4;
    constexpr u32 kEach = 3000;
    PostProducer producers[kThreads];
    Thread* threads[kThreads] = {};
    for (u32 i = 0; i < kThreads; ++i) {
        producers[i] = PostProducer{&queue, i + 10, kEach};
        threads[i] = thread_start(produce, &producers[i], 128 * 1024, nullptr);
        check(threads[i] != nullptr, "a producer thread starts");
    }
    u32 next[kThreads] = {};
    u32 received = 0;
    bool intact = true;
    const u64 start = clock_monotonic_milliseconds();
    while (received < kThreads * kEach && clock_monotonic_milliseconds() - start < 8000) {
        if (!post_queue_pop(&queue, &entry)) {
            thread_sleep(0);
            continue;
        }
        const u32 producer = entry.cause - 10;
        if (producer >= kThreads || entry.a != next[producer] || entry.b != ~entry.a)
            intact = false;
        else
            ++next[producer];
        ++received;
    }
    check(received == kThreads * kEach && intact, "posts from several threads all arrive, in order per thread");
    for (Thread* thread : threads) {
        if (thread) {
            thread_wait(thread, 2000);
            thread_close(thread);
        }
    }
}

// The generated protocol tables --------------------------------------------------------------------------------------

u32 signature_arguments(const char* signature) noexcept {
    u32 count = 0;
    for (; *signature; ++signature)
        count += !((*signature >= '0' && *signature <= '9') || *signature == '?');
    return count;
}

bool same_messages(const Message* ours, const Message* theirs, int count) noexcept {
    for (int i = 0; i < count; ++i) {
        if (!text_equal(ours[i].name, theirs[i].name) || !text_equal(ours[i].signature, theirs[i].signature))
            return false;
        const u32 arguments = signature_arguments(ours[i].signature);
        for (u32 j = 0; j < arguments; ++j) {
            const Interface* a = ours[i].types ? ours[i].types[j] : nullptr;
            const Interface* b = theirs[i].types ? theirs[i].types[j] : nullptr;
            if ((a == nullptr) != (b == nullptr) || (a && !text_equal(a->name, b->name)))
                return false;
        }
    }
    return true;
}

const Message* find_message(const Interface& interface, bool request, const char* name) noexcept {
    const Message* messages = request ? interface.methods : interface.events;
    const int count = request ? interface.method_count : interface.event_count;
    for (int i = 0; i < count; ++i)
        if (text_equal(messages[i].name, name))
            return &messages[i];
    return nullptr;
}

void test_protocol_tables() noexcept {
    // What the other tables can be compared with by hand.
    check(wl_registry_interface.method_count == 1 && text_equal(wl_registry_interface.methods[0].signature, "usun"),
          "wl_registry.bind carries the interface name and version");
    const Message* title = find_message(xdg_toplevel_interface, true, "set_title");
    check(title && text_equal(title->signature, "s"), "xdg_toplevel.set_title");
    const Message* surface = find_message(xdg_wm_base_interface, true, "get_xdg_surface");
    check(surface && text_equal(surface->signature, "no") && surface->types[0] == &xdg_surface_interface &&
              surface->types[1] == &wl_surface_interface,
          "xdg_wm_base.get_xdg_surface names its interfaces");
    const Message* shape = find_message(wp_cursor_shape_manager_v1_interface, true, "get_tablet_tool_v2");
    check(shape && shape->types[1] == &zwp_tablet_tool_v2_interface, "cursor-shape reaches the tablet tool");
    const Message* since = find_message(wl_pointer_interface, false, "axis_value120");
    check(since && text_equal(since->signature, "8ui"),
          "wl_pointer.axis_value120 is since version 8");
    check(g_protocol_interface_count >= 55, "all protocols are generated");

    // The core tables against the ones libwayland-client carries.
    SharedLibrary library{};
    if (!shared_library_open(&library, "libwayland-client.so.0")) {
        test_write_line("SKIP protocol tables against libwayland-client: not installed");
        return;
    }
    u32 compared = 0;
    for (u32 i = 0; i < g_protocol_interface_count; ++i) {
        const Interface& ours = *g_protocol_interfaces[i];
        char symbol[96];
        u32 length = 0;
        const char* const parts[] = {ours.name, "_interface"};
        for (const char* part : parts)
            for (; *part && length + 1 < sizeof(symbol); ++part)
                symbol[length++] = *part;
        symbol[length] = 0;
        const Interface* theirs = static_cast<const Interface*>(shared_library_symbol(library, symbol));
        if (!theirs)
            continue;
        ++compared;
        const int requests = minimum(ours.method_count, theirs->method_count);
        const int events = minimum(ours.event_count, theirs->event_count);
        // A newer library may add messages; the ones both know are the same, in the same order.
        if (!text_equal(ours.name, theirs->name) || !same_messages(ours.methods, theirs->methods, requests) ||
            !same_messages(ours.events, theirs->events, events)) {
            check(false, "generated table differs from libwayland-client's");
            test_write_line(ours.name);
        }
    }
    check(compared >= 20, "the core interfaces were compared with libwayland-client's");
    shared_library_close(&library);
}

// Real xkb layouts ---------------------------------------------------------------------------------------------------

struct Layout {
    XkbKeymap* keymap;
    XkbState* state;
};

Layout make_layout(const XkbApi& api, XkbContext* context, const char* layout, const char* variant) noexcept {
    const XkbRuleNames names{"evdev", "pc105", layout, variant, nullptr};
    Layout result{};
    result.keymap = api.keymap_new_from_names(context, &names, 0);
    if (result.keymap)
        result.state = api.state_new(result.keymap);
    return result;
}

void release_layout(const XkbApi& api, Layout* layout) noexcept {
    if (layout->state)
        api.state_unref(layout->state);
    if (layout->keymap)
        api.keymap_unref(layout->keymap);
}

struct Press {
    Key key;
    u32 text;
};

Press press(const XkbApi& api, const Layout& layout, u32 evdev, const char* modifier = nullptr) noexcept {
    u32 mask = 0;
    if (modifier) {
        const u32 index = api.keymap_mod_get_index(layout.keymap, modifier);
        if (index != kXkbModifierInvalid)
            mask = 1u << index;
    }
    api.state_update_mask(layout.state, mask, 0, 0, 0, 0, 0);
    u32 active = 0;
    Press result{};
    result.key = xkb_key_for(api, layout.keymap, layout.state, evdev, &active);
    result.text = api.state_key_get_utf32(layout.state, evdev + kEvdevToXkb);
    return result;
}

void test_xkb_layouts() noexcept {
    const XkbApi* api = xkb_api();
    XkbContext* context = api ? api->context_new(0) : nullptr;
    if (!context) {
        test_write_line("SKIP xkb layouts: libxkbcommon is not installed");
        return;
    }
    api->context_set_log_level(context, kXkbLogLevelCritical);
    Layout us = make_layout(*api, context, "us", nullptr);
    if (!us.keymap) {
        test_write_line("SKIP xkb layouts: no keyboard layout data (xkeyboard-config)");
        api->context_unref(context);
        return;
    }
    Layout de = make_layout(*api, context, "de", nullptr);
    Layout fr = make_layout(*api, context, "fr", nullptr);
    Layout dvorak = make_layout(*api, context, "us", "dvorak");
    Layout ru = make_layout(*api, context, "ru", nullptr);
    Layout jp = make_layout(*api, context, "jp", nullptr);
    check(de.keymap && fr.keymap && dvorak.keymap && ru.keymap && jp.keymap, "layouts compile");
    if (de.keymap && fr.keymap && dvorak.keymap && ru.keymap && jp.keymap) {
        Press p = press(*api, us, 30);
        check(p.key == 'A' && p.text == 'a', "xkb us a");
        p = press(*api, us, 30, "Shift");
        check(p.key == 'A' && p.text == 'A', "xkb us shifted a");
        p = press(*api, us, 30, "Control");
        check(p.key == 'A' && p.text == 0x01, "xkb us ctrl+a types the control code");
        p = press(*api, us, 111);
        check(p.key == key::del && p.text == 0x7F && !is_message_character(p.text), "xkb delete types no text");
        p = press(*api, us, 28);
        check(p.key == key::enter && p.text == '\r', "xkb return");
        p = press(*api, us, 14);
        check(p.key == key::backspace && p.text == '\b', "xkb backspace");
        p = press(*api, us, 79);
        check(p.key == key::end, "xkb keypad 1 without Num Lock is End");
        p = press(*api, us, 79, "Mod2");
        check(p.key == key::numpad1 && p.text == '1', "xkb keypad 1 with Num Lock");
        p = press(*api, us, 42);
        check(p.key == key::shift, "xkb left shift");
        p = press(*api, us, 12);
        check(p.key == key::minus && p.text == '-', "xkb us minus");

        p = press(*api, de, 21);
        check(p.key == 'Z' && p.text == 'z', "xkb de y position types z");
        p = press(*api, de, 44);
        check(p.key == 'Y' && p.text == 'y', "xkb de z position types y");
        p = press(*api, de, 26);
        check(p.key == key::left_bracket && p.text == 0xFC, "xkb de u-umlaut");
        const u32 level_three = api->keymap_mod_get_index(de.keymap, "LevelThree") != kXkbModifierInvalid
                                    ? api->keymap_mod_get_index(de.keymap, "LevelThree")
                                    : api->keymap_mod_get_index(de.keymap, "Mod5");
        check(level_three != kXkbModifierInvalid, "xkb de has an AltGr modifier");
        api->state_update_mask(de.state, 1u << level_three, 0, 0, 0, 0, 0);
        check(api->state_key_get_utf32(de.state, 16 + kEvdevToXkb) == '@', "xkb de AltGr+q types @");
        p = press(*api, de, 100);
        check(p.key == key::right_alt, "xkb de AltGr key");

        p = press(*api, fr, 2);
        check(p.key == '1' && p.text == '&', "xkb fr digit row unshifted");
        p = press(*api, fr, 2, "Shift");
        check(p.key == '1' && p.text == '1', "xkb fr digit row shifted");
        p = press(*api, fr, 16);
        check(p.key == 'A' && p.text == 'a', "xkb fr q position types a");
        p = press(*api, fr, 30);
        check(p.key == 'Q' && p.text == 'q', "xkb fr a position types q");

        p = press(*api, dvorak, 31);
        check(p.key == 'O' && p.text == 'o', "xkb dvorak o");
        p = press(*api, dvorak, 16);
        check(p.key == key::quote && p.text == '\'', "xkb dvorak apostrophe");
        p = press(*api, dvorak, 12);
        check(p.key == key::left_bracket && p.text == '[', "xkb dvorak left bracket");

        p = press(*api, ru, 30);
        check(p.key == 'A' && p.text == 0x444, "xkb ru ef on the a key");
        p = press(*api, ru, 2);
        check(p.key == '1' && p.text == '1', "xkb ru digit");

        p = press(*api, jp, 124);
        check(p.key == key::backslash, "xkb jp yen key");
        p = press(*api, jp, 30);
        check(p.key == 'A' && p.text == 'a', "xkb jp a");
    }
    release_layout(*api, &us);
    release_layout(*api, &de);
    release_layout(*api, &fr);
    release_layout(*api, &dvorak);
    release_layout(*api, &ru);
    release_layout(*api, &jp);

    // Compose: a dead key and a letter make one accented character.
    XkbComposeTable* table = api->compose_table_new_from_locale(context, "en_US.UTF-8", 0);
    if (!table) {
        test_write_line("SKIP compose: no compose table for en_US.UTF-8");
    } else {
        XkbComposeState* state = api->compose_state_new(table, 0);
        const u32 dead_acute = 0xFE51;
        check(api->compose_state_feed(state, dead_acute) == kXkbComposeFeedAccepted &&
                  api->compose_state_get_status(state) == kXkbComposeStatusComposing,
              "dead acute starts a sequence");
        check(api->compose_state_feed(state, 'a') == kXkbComposeFeedAccepted &&
                  api->compose_state_get_status(state) == kXkbComposeStatusComposed,
              "dead acute and a compose");
        char text[16] = {};
        const int length = api->compose_state_get_utf8(state, text, sizeof(text));
        check(length == 2 && static_cast<u8>(text[0]) == 0xC3 && static_cast<u8>(text[1]) == 0xA1, "composes a-acute");
        api->compose_state_reset(state);
        check(api->compose_state_feed(state, 'x') == kXkbComposeFeedAccepted &&
                  api->compose_state_get_status(state) == kXkbComposeStatusNothing,
              "an ordinary key composes nothing");
        api->compose_state_unref(state);
        api->compose_table_unref(table);
    }
    api->context_unref(context);
}

// The desktop settings as PlatformSettings ----------------------------------------------------------------------------

void test_settings_map() noexcept {
    dbus::DesktopSettings desktop;
    dbus::desktop_settings_defaults(&desktop);
    PlatformSettings settings = platform_settings_from_desktop(desktop);
    check(settings.double_click_ms == 500 && settings.double_click_dx_px == 4 && settings.caret_blink_ms == 530 &&
              settings.wheel_lines == 3 && settings.client_area_animation && !settings.dark_scheme &&
              settings.system_idle_ms == kSystemIdleUnknown,
          "settings: a desktop that publishes nothing keeps the defaults");

    desktop.loaded = true;
    desktop.color_scheme = dbus::ColorScheme::dark;
    desktop.double_click_ms = 350;
    desktop.cursor_blink_ms = 1000;
    settings = platform_settings_from_desktop(desktop);
    check(settings.dark_scheme && settings.double_click_ms == 350 && settings.caret_blink_ms == 500,
          "settings: the dark scheme, the double-click time, and the blink cycle halved");
    desktop.color_scheme = dbus::ColorScheme::light;
    check(!platform_settings_from_desktop(desktop).dark_scheme, "settings: light is not dark");
    desktop.color_scheme = dbus::ColorScheme::no_preference;
    check(!platform_settings_from_desktop(desktop).dark_scheme, "settings: no preference is not dark");

    desktop.cursor_blink = false;
    check(platform_settings_from_desktop(desktop).caret_blink_ms == kCaretNoBlink, "settings: no blink is INFINITE");
    desktop.cursor_blink = true;
    desktop.cursor_blink_ms = 120;
    check(platform_settings_from_desktop(desktop).caret_blink_ms == 100, "settings: the blink time has a floor");

    desktop.animations = true;
    desktop.reduced_motion = true;
    check(!platform_settings_from_desktop(desktop).client_area_animation, "settings: reduced motion turns animation off");
    desktop.reduced_motion = false;
    desktop.animations = false;
    check(!platform_settings_from_desktop(desktop).client_area_animation, "settings: enable-animations off turns it off");
    desktop.animations = true;
    check(platform_settings_from_desktop(desktop).client_area_animation, "settings: animation on");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_wayland_input_entry) {
    maicord::test_key_table();
    maicord::test_characters();
    maicord::test_double_click();
    maicord::test_key_repeat();
    maicord::test_protocol_tables();
    maicord::test_xkb_layouts();
    maicord::test_uri();
    maicord::test_post_queue();
    maicord::test_settings_map();
    maicord::test_exit(maicord::g_failures, "wayland_input");
}

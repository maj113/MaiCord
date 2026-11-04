#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// What the tests of the desktop services share: the control connection to the python mock (tools/test_dbus.sh), its request
// log, text helpers, and pumping of the session bus.

namespace maicord::mock {

using namespace dbus;

inline u32 g_failures;
inline Connection* g_control;

inline void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

inline u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

inline bool text_equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return a == b;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

inline bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix && *text == *prefix)
        ++text, ++prefix;
    return *prefix == 0;
}

inline bool contains(const char* text, const char* needle) noexcept {
    if (!needle[0])
        return false;
    for (u32 i = 0; text[i]; ++i)
        if (starts_with(text + i, needle))
            return true;
    return false;
}

inline CallTarget target(const char* member) noexcept {
    return CallTarget{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", member};
}

inline void control(const char* member, const Builder* body) noexcept {
    Message reply;
    const Status status = connection_call_sync(g_control, target(member), body, 3000, &reply);
    check(status == Status::ok, member);
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
}

inline void set_mode(const char* name, bool value) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, name);
    builder_bool(&b, value);
    control("SetMode", &b);
    builder_release(&b);
}

// The mock's request log, accumulated: take_log appends what the mock logged since the last take, clear_log forgets it all.
inline constexpr u32 kLogCapacity = 96;
inline char g_lines[kLogCapacity][1536];
inline u32 g_line_count;

inline void take_log() noexcept {
    Message reply;
    if (connection_call_sync(g_control, target("Take"), nullptr, 3000, &reply) != Status::ok)
        return;
    Reader r;
    message_reader(reply, &r);
    if (reader_enter_array(&r)) {
        while (reader_more(r) && g_line_count < kLogCapacity) {
            const char* text = nullptr;
            u32 length = 0;
            if (!reader_string(&r, &text, &length))
                break;
            if (length >= sizeof(g_lines[0]))
                length = sizeof(g_lines[0]) - 1;
            copy_bytes(g_lines[g_line_count], text, length);
            g_lines[g_line_count][length] = 0;
            ++g_line_count;
        }
    }
    message_release(&reply);
}

inline void clear_log() noexcept {
    take_log();
    g_line_count = 0;
}

inline const char* find_line(const char* prefix) noexcept {
    for (u32 i = 0; i < g_line_count; ++i)
        if (starts_with(g_lines[i], prefix))
            return g_lines[i];
    return nullptr;
}

// Every line of the accumulated log, for the failure of an assertion about it.
inline void dump_log() noexcept {
    for (u32 i = 0; i < g_line_count; ++i) {
        test_write_text("  log: ");
        test_write_line(g_lines[i]);
    }
}

inline u32 count_lines(const char* prefix) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < g_line_count; ++i)
        count += starts_with(g_lines[i], prefix);
    return count;
}

// Pumps the session bus until `done` or the time passes.
inline bool pump(bool (*done)(), u32 milliseconds) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    for (;;) {
        if (done())
            return true;
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return false;
        Waitable waitable = session_bus_waitable();
        u32 wait = session_bus_wait_ms(now);
        const u32 left = static_cast<u32>(deadline - now);
        if (wait > left)
            wait = left;
        if (wait > 50)
            wait = 50;  // the predicate may depend on something that is not bus traffic (a child process ending)
        if (waitable) {
            wait_many(&waitable, 1, wait, 0);
        } else {
            const TimeSpec nap{static_cast<i64>(wait / 1000), static_cast<i64>(wait % 1000) * 1000000};
            sys_ppoll(nullptr, 0, &nap);
        }
        session_bus_process(clock_monotonic_milliseconds());
    }
}

inline void settle(u32 milliseconds) noexcept {
    const u64 until = clock_monotonic_milliseconds() + milliseconds;
    while (clock_monotonic_milliseconds() < until) {
        Waitable waitable = session_bus_waitable();
        if (waitable)
            wait_many(&waitable, 1, 20, 0);
        session_bus_process(clock_monotonic_milliseconds());
    }
}

// Asks the mock to call a method of the code under test (Invoke): invoke_begin, the arguments with the arg_ helpers (each
// writes one variant of the av array), invoke_end. The answer is the log line `Result <tag> ok|error ...`, which wait_result
// finds while the session bus is pumped. A destination of "@item" is the service last registered with the watcher.
inline void invoke_begin(Builder* b, const char* tag, const char* destination, const char* path, const char* interface,
                         const char* member, const char* signature) noexcept {
    builder_init(b);
    builder_string(b, tag);
    builder_string(b, destination);
    builder_string(b, path);
    builder_string(b, interface);
    builder_string(b, member);
    builder_string(b, signature);
    builder_open_array(b, "v");
}

inline void invoke_end(Builder* b) noexcept {
    builder_close_array(b);
    control("Invoke", b);
    builder_release(b);
}

inline void arg_i32(Builder* b, i32 value) noexcept {
    builder_open_variant(b, "i");
    builder_i32(b, value);
    builder_close_variant(b);
}

inline void arg_u32(Builder* b, u32 value) noexcept {
    builder_open_variant(b, "u");
    builder_u32(b, value);
    builder_close_variant(b);
}

inline void arg_string(Builder* b, const char* value) noexcept {
    builder_open_variant(b, "s");
    builder_string(b, value);
    builder_close_variant(b);
}

inline void arg_string_array(Builder* b, const char* const* values, u32 count) noexcept {
    builder_open_variant(b, "as");
    builder_open_array(b, "s");
    for (u32 i = 0; i < count; ++i)
        builder_string(b, values[i]);
    builder_close_array(b);
    builder_close_variant(b);
}

inline void arg_int_array(Builder* b, const i32* values, u32 count) noexcept {
    builder_open_variant(b, "ai");
    builder_open_array(b, "i");
    for (u32 i = 0; i < count; ++i)
        builder_i32(b, values[i]);
    builder_close_array(b);
    builder_close_variant(b);
}

// a{sv}: empty, or with the platform data's activation-token.
inline void arg_platform_data(Builder* b, const char* token) noexcept {
    builder_open_variant(b, "a{sv}");
    builder_open_array(b, "{sv}");
    if (token) {
        builder_open_entry(b);
        builder_string(b, "activation-token");
        builder_open_variant(b, "s");
        builder_string(b, token);
        builder_close_variant(b);
        builder_close_entry(b);
    }
    builder_close_array(b);
    builder_close_variant(b);
}

// a(isvu) with one "clicked" event of `id`.
inline void arg_click_group(Builder* b, i32 id) noexcept {
    builder_open_variant(b, "a(isvu)");
    builder_open_array(b, "(isvu)");
    builder_open_struct(b, "(isvu)");
    builder_i32(b, id);
    builder_string(b, "clicked");
    builder_open_variant(b, "i");
    builder_i32(b, 0);
    builder_close_variant(b);
    builder_u32(b, 0);
    builder_close_struct(b);
    builder_close_array(b);
    builder_close_variant(b);
}

// The log line `Result <tag> ` once the mock has logged it (the session bus is pumped meanwhile); null on timeout. The log
// keeps every line, so the lines before it can still be searched.
inline const char* wait_result(const char* tag, u32 milliseconds) noexcept {
    char prefix[64] = "Result ";
    copy_bytes(prefix + 7, tag, text_length(tag) + 1);
    prefix[7 + text_length(tag)] = ' ';
    prefix[8 + text_length(tag)] = 0;
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    for (;;) {
        take_log();
        if (const char* line = find_line(prefix))
            return line;
        if (clock_monotonic_milliseconds() >= deadline)
            return nullptr;
        settle(30);
    }
}

// A call to the code under test whose answer is expected as exactly `expected` after "Result <tag> ".
inline bool result_is(const char* tag, const char* expected, u32 milliseconds = 3000) noexcept {
    const char* line = wait_result(tag, milliseconds);
    char want[1600] = "Result ";
    u32 length = 7;
    copy_bytes(want + length, tag, text_length(tag));
    length += text_length(tag);
    want[length++] = ' ';
    copy_bytes(want + length, expected, text_length(expected) + 1);
    const bool ok = line && text_equal(line, want);
    if (!ok) {
        test_write_text("  got:  ");
        test_write_line(line ? line : "(no result)");
        test_write_text("  want: ");
        test_write_line(want);
    }
    return ok;
}

} // namespace maicord::mock

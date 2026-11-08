// Text-to-speech through speech-dispatcher (src/platform/audio/speechd). The logic runs against a table of fake libspeechd
// functions: what is opened and said, in which order, the text handed over (a C string, cut at a character boundary, the
// valid prefix), and the failure rules. Three more cases run the real functions:
//
//   --no-library   MAICORD_SPEECHD_LIBRARY names a library that does not exist (a machine without speech-dispatcher)
//   --no-daemon    libspeechd loads, but no daemon is reachable or startable (skipped without libspeechd)
//   --live         a speech-dispatcher with the dummy output module listens at $SPEECHD_ADDRESS (tools/test_speechd.sh
//                  starts it and reads its log afterwards; skipped without libspeechd)
//
// The cases set the variables themselves (cmake/test_registry.cmake, tools/test_speechd.sh).
#include "../../../src/platform/audio/speechd/speechd.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/sys/linux/syscall.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same_text(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

// The fake daemon connection ---------------------------------------------------------------------------------------------

struct Fake {
    u32 loads, opens, closes, says, cancels;
    bool open_fails;
    i32 say_result;
    char order[64]; // 'o'pen, 'x' cancel, 's'ay, 'c'lose
    u32 order_count;
    char client[32], connection_name[32];
    i32 mode, autospawn, priority;
    bool user_null, address_null;
    char said[speechd::kTextMaximumBytes + 16];
    u32 said_bytes;
};
Fake g_fake;

void note(char what) noexcept {
    if (g_fake.order_count < sizeof(g_fake.order))
        g_fake.order[g_fake.order_count++] = what;
}

void copy_text(char* destination, u32 capacity, const char* text) noexcept {
    u32 i = 0;
    for (; text && text[i] && i + 1 < capacity; ++i)
        destination[i] = text[i];
    destination[i] = 0;
}

speechd::Connection* fake_open(const char* client, const char* connection, const char* user, i32 mode,
                               const speechd::Address* address, i32 autospawn, char** error) noexcept {
    ++g_fake.opens;
    note('o');
    copy_text(g_fake.client, sizeof(g_fake.client), client);
    copy_text(g_fake.connection_name, sizeof(g_fake.connection_name), connection);
    g_fake.user_null = user == nullptr;
    g_fake.address_null = address == nullptr;
    g_fake.mode = mode;
    g_fake.autospawn = autospawn;
    if (g_fake.open_fails) {
        // libspeechd hands the message over malloc()ed: the caller frees it.
        static const char message[] = "Error: Can't connect to Speech Dispatcher";
        char* copy = static_cast<char*>(malloc(sizeof(message)));
        if (copy)
            copy_bytes(copy, message, sizeof(message));
        *error = copy;
        return nullptr;
    }
    *error = nullptr;
    return reinterpret_cast<speechd::Connection*>(&g_fake);
}

void fake_close(speechd::Connection* connection) noexcept {
    check(connection == reinterpret_cast<speechd::Connection*>(&g_fake), "close gets the connection that was opened");
    ++g_fake.closes;
    note('c');
}

i32 fake_say(speechd::Connection*, i32 priority, const char* text) noexcept {
    ++g_fake.says;
    note('s');
    g_fake.priority = priority;
    u32 length = 0;
    while (text[length] && length + 1 < sizeof(g_fake.said)) {
        g_fake.said[length] = text[length];
        ++length;
    }
    g_fake.said[length] = 0;
    g_fake.said_bytes = length;
    return g_fake.say_result;
}

i32 fake_cancel(speechd::Connection*) noexcept {
    ++g_fake.cancels;
    note('x');
    return 0;
}

const speechd::Api g_fake_api = {fake_open, fake_close, fake_say, fake_cancel};

const speechd::Api* load_fake() noexcept {
    ++g_fake.loads;
    return &g_fake_api;
}

const speechd::Api* load_missing() noexcept {
    ++g_fake.loads;
    return nullptr;
}

bool order_is(const char* expected) noexcept {
    g_fake.order[g_fake.order_count < sizeof(g_fake.order) ? g_fake.order_count : sizeof(g_fake.order) - 1] = 0;
    return same_text(g_fake.order, expected);
}

void forget() noexcept {
    clear_bytes(&g_fake, static_cast<u32>(sizeof(g_fake)));
    g_fake.say_result = 1;
}

bool said(const char* text) noexcept {
    return same_text(g_fake.said, text);
}

Utf8View view(const char* text, u32 bytes) noexcept {
    return Utf8View{text, bytes};
}

void test_logic() noexcept {
    forget();
    Speech speech{};
    check(!speechd::speak(&speech, load_fake, {}) && !speechd::speak(&speech, load_fake, view("x", 0)) &&
              !speechd::speak(nullptr, load_fake, view("x", 1)) && g_fake.loads == 0 && g_fake.opens == 0 &&
              !speech.attempted,
          "no text and no speech say nothing and open nothing");

    // The first request opens the client, cancels what it was saying and queues the text.
    check(speechd::speak(&speech, load_fake, view("hello world", 11)) && speech.voice && speech.attempted,
          "the first text opens a connection and is accepted");
    check(g_fake.opens == 1 && same_text(g_fake.client, "maicord") && g_fake.mode == speechd::kModeSingle &&
              g_fake.autospawn == 1 && g_fake.user_null && g_fake.address_null,
          "the connection is the client maicord, single threaded, default address, autospawning");
    check(order_is("oxs") && g_fake.priority == speechd::kPriorityText && said("hello world"),
          "the text is queued as text after the client's earlier speech is cancelled");

    // A new request interrupts the previous one on the same connection.
    check(speechd::speak(&speech, load_fake, view("second", 6)) && g_fake.opens == 1 && order_is("oxsxs") && said("second"),
          "a second text cancels the first and reuses the connection");

    // The view is not a C string: the daemon gets a terminated copy, cut at the first NUL.
    check(speechd::speak(&speech, load_fake, view("abc\0def", 7)) && said("abc"), "text stops at a NUL");
    const u32 says = g_fake.says;
    check(!speechd::speak(&speech, load_fake, view("\0abc", 4)) && g_fake.says == says, "text that starts with a NUL is no text");

    // Text that is not UTF-8 is cut before the first bad byte; nothing is said when that is the start.
    check(speechd::speak(&speech, load_fake, view("ab\xFF" "cd", 5)) && said("ab"), "invalid UTF-8 is cut before the bad byte");
    check(!speechd::speak(&speech, load_fake, view("\xFFzz", 3)) && g_fake.says == says + 1,
          "text that starts with a bad byte is no text");
    check(speechd::speak(&speech, load_fake, view("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80", 14)) &&
              said("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"),
          "multi-byte text goes through whole");

    // Long text is cut at the maximum, never inside a character.
    static char big[speechd::kTextMaximumBytes + 16];
    for (char& c : big)
        c = 'a';
    check(speechd::speak(&speech, load_fake, view(big, speechd::kTextMaximumBytes + 10)) &&
              g_fake.said_bytes == speechd::kTextMaximumBytes,
          "long text is cut at the maximum");
    big[speechd::kTextMaximumBytes - 1] = static_cast<char>(0xC3);
    big[speechd::kTextMaximumBytes] = static_cast<char>(0xA9);
    check(speechd::speak(&speech, load_fake, view(big, speechd::kTextMaximumBytes + 10)) &&
              g_fake.said_bytes == speechd::kTextMaximumBytes - 1,
          "and not through a two-byte character");

    // The daemon went away: the failed say drops the connection and the next text opens another.
    const u32 opens = g_fake.opens, closes = g_fake.closes;
    g_fake.say_result = -1;
    check(!speechd::speak(&speech, load_fake, view("lost", 4)) && !speech.voice && !speech.attempted &&
              g_fake.closes == closes + 1,
          "a failed say closes the connection");
    g_fake.say_result = 1;
    check(speechd::speak(&speech, load_fake, view("back", 4)) && g_fake.opens == opens + 1 && said("back"),
          "the next text opens a new connection");

    // Release stops the speech and closes the connection; the speech is closed again.
    forget();
    speechd::release(&speech, &g_fake_api);
    check(order_is("xc") && !speech.voice && !speech.attempted, "release cancels, closes and resets");
    forget();
    speechd::release(&speech, &g_fake_api);
    speechd::release(nullptr, &g_fake_api);
    check(g_fake.order_count == 0, "releasing a closed speech calls nothing");
}

void test_failures() noexcept {
    // No daemon: the open fails, is logged, and is not tried again (a click would otherwise wait on the spawn each time).
    forget();
    g_fake.open_fails = true;
    Speech speech{};
    check(!speechd::speak(&speech, load_fake, view("hello", 5)) && !speech.voice && speech.attempted && g_fake.opens == 1 &&
              g_fake.says == 0 && g_fake.cancels == 0,
          "no daemon: speech is unavailable");
    g_fake.open_fails = false;
    check(!speechd::speak(&speech, load_fake, view("hello", 5)) && g_fake.opens == 1, "and the open is not retried");
    speechd::release(&speech, &g_fake_api);
    check(g_fake.closes == 0 && !speech.attempted, "releasing it closes nothing and allows a new attempt");
    check(speechd::speak(&speech, load_fake, view("hello", 5)) && g_fake.opens == 2, "the attempt after a release opens again");
    speechd::release(&speech, &g_fake_api);

    // No library: nothing is opened.
    forget();
    Speech none{};
    check(!speechd::speak(&none, load_missing, view("hello", 5)) && !none.voice && g_fake.loads == 1 && g_fake.opens == 0,
          "no libspeechd: speech is unavailable");
    speechd::release(&none, nullptr);
}

// The real library -------------------------------------------------------------------------------------------------------

void test_no_library() noexcept {
    check(speechd::library() == nullptr, "a missing libspeechd is not loaded");
    Speech speech{};
    check(!speech_speak(&speech, view("hello", 5)) && !speech.voice, "no libspeechd: speech reports unavailable");
    check(!speech_speak(&speech, view("hello", 5)), "and keeps reporting it");
    speech_release(&speech);
    check(!speech.attempted && !speech.voice, "releasing it resets it");
}

[[noreturn]] void skip(const char* reason) noexcept {
    test_write_text("SKIP ");
    test_write_line(reason);
    sys_exit_group(77);
}

void test_no_daemon() noexcept {
    if (!speechd::library())
        skip("libspeechd.so.2 is not installed");
    Speech speech{};
    check(!speech_speak(&speech, view("hello", 5)) && !speech.voice && speech.attempted,
          "libspeechd without a daemon: speech reports unavailable");
    speech_release(&speech);
    check(!speech.attempted, "and releasing it resets it");
}

void test_live() noexcept {
    if (!speechd::library())
        skip("libspeechd.so.2 is not installed");
    const char* address = environment_get("SPEECHD_ADDRESS");
    if (!address || !*address)
        skip("SPEECHD_ADDRESS is not set: this test is run by tools/test_speechd.sh");
    Speech speech{};
    check(speech_speak(&speech, view("Alice said first message", 24)) && speech.voice, "a speech-dispatcher takes the first text");
    check(speech_speak(&speech, view("Bob said second message", 23)), "and the second, which interrupts it");
    check(!speech_speak(&speech, view("\xFF", 1)), "a text that is not UTF-8 is refused");
    check(speech_speak(&speech, view("caf\xC3\xA9 \xE2\x82\xAC", 9)), "multi-byte text is taken");
    speech_release(&speech);
    check(!speech.voice && !speech.attempted, "release closes the connection");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_speechd_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    bool no_library = false, no_daemon = false, live = false;
    for (int i = 1; i < startup.argc; ++i) {
        no_library |= same_text(startup.argv[i], "--no-library");
        no_daemon |= same_text(startup.argv[i], "--no-daemon");
        live |= same_text(startup.argv[i], "--live");
    }
    if (no_library) {
        test_no_library();
        test_exit(g_failures, "speechd_no_library");
    }
    if (no_daemon) {
        test_no_daemon();
        test_exit(g_failures, "speechd_no_daemon");
    }
    if (live) {
        test_live();
        test_exit(g_failures, "speechd_live");
    }
    test_logic();
    test_failures();
    test_exit(g_failures, "speechd");
}

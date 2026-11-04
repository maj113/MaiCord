// The Linux counterpart of tests/test_main.cpp: the expectation sink and the ordered run of the core tests the Linux
// build has ported (the order of tests/core_test_cases.inc; cmake/linux_test_registry.cmake lists the sources).
#include "test_main_shared.hpp"

#include "test_output.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

} // namespace

void expect(bool condition, const char* name) noexcept {
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_text(name);
        test_write_text("\n");
    }
}

// Defined in tests/core/mailbox_release_race.cpp.
void test_mailbox_release_races(void (*check)(bool, const char*) noexcept) noexcept;

} // namespace maicord

MAICORD_TEST_ENTRY(maicord_tests_entry) {
    maicord::test_runtime_memory();
    maicord::test_deadline_policy();
    maicord::test_spsc_queue();
    maicord::test_spsc_mailbox();
    maicord::test_trace_record();
    maicord::test_byte_queue();
    maicord::test_clock_microseconds();
    maicord::test_mailbox_acquire_release(maicord::expect);
    maicord::test_mailbox_release_races(maicord::expect);
    maicord::test_json_string_decode(maicord::expect);
    maicord::test_json_reader_visit(maicord::expect);
    maicord::test_log_buffered_flush();
    maicord::test_arena();
    maicord::test_stable_storage();
    maicord::test_utf8();
    maicord::test_json_scan();

    if (maicord::g_failures == 0) {
        maicord::test_write_text("PASS maicord_core\n");
    }
    maicord::test_process_exit(maicord::g_failures);
}

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace {
void (*after_release)(volatile LONG*) noexcept;
} // namespace

// Distinct namespace prevents instrumented template instantiations from
// replacing production ones in this test executable. The exact-match overload
// below wins over the header's template for the 32-bit descriptor counters:
// publication itself is unchanged, and the hook schedules a legal producer
// action immediately afterward.
namespace mailbox_release_test {
using namespace maicord;
void spsc_mailbox_store(volatile LONG* sequence, LONG value) noexcept {
    *sequence = value;
    if (after_release)
        after_release(sequence);
}
} // namespace mailbox_release_test
#define maicord mailbox_release_test
#include "maicord/core/spsc_mailbox.hpp"
#undef maicord

namespace {
mailbox_release_test::SpscMailbox<maicord::u32, 2, 32> mailbox{};
bool reused;
void reuse(volatile LONG* address) noexcept {
    if (address != &mailbox.read_sequence)
        return;
    after_release = nullptr;
    const maicord::u8 replacement[7]{9, 8, 7, 6, 5, 4, 3};
    reused = mailbox_release_test::spsc_mailbox_push(&mailbox, 3u, {replacement, 7}).ok();
}
} // namespace

namespace maicord {
void test_mailbox_release_races(void (*check)(bool, const char*) noexcept) noexcept {
    for (u32 discard = 0; discard < 2; ++discard) {
        mailbox = {};
        reused = false;
        const u8 bytes[5]{1, 2, 3, 4, 5};
        check(mailbox_release_test::spsc_mailbox_push(&mailbox, 1u, {bytes, 3}).ok() &&
                  mailbox_release_test::spsc_mailbox_push(&mailbox, 2u, {bytes, 5}).ok(),
              "mailbox race: fill descriptors");
        u32 value = 0;
        u8 output[8]{};
        after_release = reuse;
        const auto result = discard ? mailbox_release_test::spsc_mailbox_discard(&mailbox, &value)
                                    : mailbox_release_test::spsc_mailbox_pop(&mailbox, &value, {output, 8});
        after_release = nullptr;
        check(reused && result.ok() && result.payload_bytes == 3 && value == 1 &&
                  (discard || bytes_equal(output, bytes, 3)),
              discard ? "mailbox discard snapshots length before reuse"
                      : "mailbox pop snapshots length before reuse");
    }
}
} // namespace maicord

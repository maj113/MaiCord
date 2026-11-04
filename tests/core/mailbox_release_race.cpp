#include "maicord/core/slice.hpp"
#include "maicord/core/interlocked.hpp"

namespace {
void (*after_release)(volatile maicord::Atomic32*) noexcept;
} // namespace

// Distinct namespace prevents instrumented template instantiations from
// replacing production ones in this test executable. The exact-match overload
// below wins over the header's template for the 32-bit descriptor counters:
// publication itself is unchanged, and the hook schedules a legal producer
// action immediately afterward.
namespace mailbox_release_test {
using namespace maicord;
void spsc_mailbox_store(volatile Atomic32* sequence, Atomic32 value) noexcept {
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
void reuse(volatile maicord::Atomic32* address) noexcept {
    if (address != &mailbox.read_sequence)
        return;
    after_release = nullptr;
    const maicord::u8 replacement[7]{9, 8, 7, 6, 5, 4, 3};
    reused = mailbox_release_test::spsc_mailbox_push(&mailbox, 3u, {replacement, 7}).ok();
}
} // namespace

namespace maicord {
void test_mailbox_release_races(void (*check)(bool, const char*) noexcept) noexcept {
    mailbox = {};
    reused = false;
    const u8 bytes[5]{1, 2, 3, 4, 5};
    check(mailbox_release_test::spsc_mailbox_push(&mailbox, 1u, {bytes, 3}).ok() &&
              mailbox_release_test::spsc_mailbox_push(&mailbox, 2u, {bytes, 5}).ok(),
          "mailbox race: fill descriptors");
    u32 value = 0;
    u8 output[8]{};
    after_release = reuse;
    const auto result = mailbox_release_test::spsc_mailbox_pop(&mailbox, &value, {output, 8});
    after_release = nullptr;
    check(reused && result.ok() && result.payload_bytes == 3 && value == 1 && bytes_equal(output, bytes, 3),
          "mailbox pop snapshots length before reuse");
}
} // namespace maicord

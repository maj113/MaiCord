#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_sender_vectors.hpp"
constexpr DaveReceiveIdentity identity{101, 202};
void seed(u32 epoch, u8* output) noexcept {
    for (u32 i = 0; i != 16; ++i)
        output[i] = static_cast<u8>(i + epoch * 31);
}
u32 wire(Check check, u32 epoch, u32 counter, u8* output) noexcept {
    for (const auto& vector : sender_vectors)
        if (vector.epoch == epoch && vector.counter == counter)
            return test_unhex(vector.wire, output);
    check(false, "DAVE sender fixture exists");
    return 0;
}
void correct(Check check, const DaveSenderOpenResult& result, u32 epoch, const u8* output) noexcept {
    bool matched = result.ok() && result.bytes == 48 && result.epoch == DaveEpochError::none;
    for (u32 i = 0; i != 48 && matched; ++i)
        matched = output[i] == static_cast<u8>(i * 17 + epoch * 7 + 9);
    check(matched, "DAVE sender authenticates/reconstructs independent mixed-clear epoch fixture");
}
void transition(Check check, DaveReceiveSender& sender, u32 epoch, u64 revision, u64 now, u64 grace,
                Gcm64Backend backend) noexcept {
    u8 material[16];
    seed(epoch, material);
    check(dave_receive_sender_transition(sender, identity, revision, {material, 16}, now, grace, backend)
              .ok(),
          "DAVE sender transition initializes replacement before publishing");
}
DaveSenderOpenResult open(Check check, DaveReceiveSender& sender, KdfSha256Workspace& workspace, u32 epoch,
                          u32 counter, u64 now) noexcept {
    u8 frame[64], output[64], scratch[64];
    const u32 size = wire(check, epoch, counter, frame);
    memset(output, 0xa5, sizeof(output));
    memset(scratch, 0xa5, sizeof(scratch));
    const auto result = dave_receive_sender_open(sender, identity, workspace, {frame, size}, now,
                                                 {scratch, 64}, {output, 64});
    if (result.ok())
        correct(check, result, epoch, output);
    check(test_filled(output + 48, 16, 0xa5) && test_filled(scratch + 48, 16, 0xa5),
          "DAVE sender attempts preserve output/scratch excess capacity");
    return result;
}
void transitions(Check check, KdfSha256Workspace& workspace, Gcm64Backend backend) noexcept {
    DaveReceiveSender sender{};
    check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
          "DAVE sender identity initialization");
    transition(check, sender, 0, 1, 0, 100, backend);
    auto result = open(check, sender, workspace, 0, 1, 0);
    check(result.ok() && result.revision == 1 && result.epochs_tried == 1 && result.crypto_attempts == 1,
          "DAVE first sender epoch succeeds directly");
    transition(check, sender, 1, 2, 10, 100, backend);
    result = open(check, sender, workspace, 0, 2, 20);
    check(result.ok() && result.revision == 1 && result.epochs_tried == 2 && result.crypto_attempts == 2,
          "DAVE tries new epoch first then authenticates reordered older epoch frame");
    result = open(check, sender, workspace, 1, 1, 20);
    check(result.ok() && result.revision == 2 && result.epochs_tried == 1 && result.crypto_attempts == 1,
          "DAVE same nonce in distinct epoch has independent replay state");
    check(!open(check, sender, workspace, 0, 2, 20).ok(),
          "DAVE old epoch duplicate remains rejected after transition");
    u8 material[16];
    seed(2, material);
    const auto failed = dave_receive_sender_transition(sender, identity, 3, {material, 16}, 40, 9999,
                                                       static_cast<Gcm64Backend>(255));
    check(failed.error == DaveSenderError::epoch_initialize && sender.count == 2 &&
              sender.latest_revision == 2 && sender.last_now_ms == 20 && sender.next_expiry_ms == 110,
          "DAVE failed replacement leaves old keys, grace, revision and clock unchanged");
    transition(check, sender, 2, 3, 50, 100, backend);
    check(sender.count == 3 && sender.next_expiry_ms == 150,
          "DAVE successful transition refreshes grace for all live previous epochs");
    result = open(check, sender, workspace, 0, 3, 120);
    check(result.ok() && result.revision == 1 && result.epochs_tried == 3,
          "DAVE oldest live epoch receives refreshed transition grace");
    check(dave_receive_sender_collect(sender, identity, 150) == DaveSenderError::none && sender.count == 3,
          "DAVE collect retains epochs at inclusive expiry boundary");
    check(dave_receive_sender_collect(sender, identity, 151) == DaveSenderError::none && sender.count == 1,
          "DAVE collect releases expired epochs even without media delivery");
    check(!open(check, sender, workspace, 0, 4, 151).ok(),
          "DAVE expired epoch cannot authenticate later frame");
    check(dave_receive_sender_transition(sender, identity, 2, {material, 16}, 151, 100).error ==
                  DaveSenderError::stale_transition &&
              sender.count == 1,
          "DAVE stale transition cannot resurrect removed epoch or reset replay");
    check(dave_receive_sender_retire(sender, identity, 4, 160, 20) == DaveSenderError::none,
          "DAVE keyless transition starts final retirement grace");
    result = open(check, sender, workspace, 2, 1, 180);
    check(result.ok() && result.revision == 3,
          "DAVE current epoch remains usable through retirement grace");
    check(dave_receive_sender_collect(sender, identity, 181) == DaveSenderError::none && !sender.count &&
              sender.next_expiry_ms == ~u64{0},
          "DAVE retirement leaves no keys or idle expiry deadline");
    check(open(check, sender, workspace, 2, 2, 181).error == DaveSenderError::no_epoch,
          "DAVE keyless retirement does not authorize plaintext");
    check(dave_receive_sender_retire(sender, identity, 4, 181, 100) == DaveSenderError::stale_transition,
          "DAVE transition revision survives removal of all epoch nodes");
    dave_receive_sender_release(&sender);
    check(!sender.initialized && !sender.newest && !sender.count && !sender.latest_revision,
          "DAVE sender teardown retires identity, revisions, replay and keys");
    dave_receive_sender_release(&sender);
}
void boundaries(Check check, KdfSha256Workspace& workspace) noexcept {
    DaveReceiveSender sender{};
    check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
          "DAVE boundary sender init");
    transition(check, sender, 0, 65535, 0, 10, Gcm64Backend::automatic);
    const auto* old_node = sender.newest;
    transition(check, sender, 1, 65536, 1, 10, Gcm64Backend::automatic);
    u8 frame[64], output[64], scratch[64], material[16];
    wire(check, 0, 1, frame);
    seed(2, material);
    memset(output, 0xa5, sizeof(output));
    check(dave_receive_sender_open(sender, {102, 202}, workspace, {nullptr, 1}, 99, {}, {}).error ==
                  DaveSenderError::identity_mismatch &&
              sender.last_now_ms == 1,
          "DAVE stale-call delivery is rejected before parsing or touching epoch clock");
    check(dave_receive_sender_transition(sender, {101, 203}, 65537, {material, 16}, 2, 10).error ==
              DaveSenderError::identity_mismatch,
          "DAVE cannot install another sender's transition");
    check(dave_receive_sender_collect(sender, identity, 0) == DaveSenderError::invalid_time,
          "DAVE sender rejects rollback across epoch clocks");
    const u64 expires = sender.next_expiry_ms;
    check(dave_receive_sender_transition(sender, identity, 65536, {material, 16}, 2, 1000).error ==
                  DaveSenderError::stale_transition &&
              sender.next_expiry_ms == expires,
          "DAVE duplicate revision cannot refresh grace or create a fresh replay owner");
    check(dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 12, {scratch, 64},
                                   {reinterpret_cast<u8*>(const_cast<DaveReceiveNode*>(old_node)), 64})
                      .error == DaveSenderError::invalid_input &&
              sender.count == 2,
          "DAVE protects every old node before cleanup can free aliased caller storage");
    for (u32 size = 0; size != 48; ++size) {
        check(dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 2, {scratch, 64},
                                       {output, size})
                          .error == DaveSenderError::output_capacity &&
                  dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 2, {scratch, size},
                                           {output, 64})
                          .error == DaveSenderError::scratch_capacity &&
                  test_filled(output, 64, 0xa5),
              "DAVE short shared buffers fail before trying any epoch");
    }
    check(dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 2, {scratch, 64},
                                   {frame + 1, 63})
                  .error == DaveSenderError::invalid_input,
          "DAVE sender rejects partial frame/output overlap");
    u8 original[64];
    copy_bytes(original, frame, 64);
    const auto in_place =
        dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 2, {scratch, 64}, {frame, 64});
    correct(check, in_place, 0, frame);
    check(in_place.revision == 65535 && in_place.epochs_tried == 2 &&
              bytes_equal(frame + 48, original + 48, 16),
          "DAVE sender retries mixed-clear frame in place and preserves footer");
    check(dave_receive_sender_collect(sender, identity, 12) == DaveSenderError::none && sender.count == 1,
          "DAVE old epoch collection after guarded input operations");
    transition(check, sender, 2, 65537, 13, 100, Gcm64Backend::automatic);
    check(sender.count == 2, "DAVE next transition does not resurrect expired/collected old epoch");
    wire(check, 0, 2, frame);
    check(
        !dave_receive_sender_open(sender, identity, workspace, {frame, 64}, 14, {scratch, 64}, {output, 64})
             .ok(),
        "DAVE no retained key can authenticate removed epoch");
    dave_receive_sender_release(&sender);
    check(dave_receive_sender_initialize(&sender, {999, 202}, ~u64{0} - 5) == DaveSenderError::none,
          "DAVE replacement call initializes fresh identity");
    seed(0, material);
    check(dave_receive_sender_transition(sender, {999, 202}, 1, {material, 16}, ~u64{0} - 5, 0).ok() &&
              dave_receive_sender_retire(sender, {999, 202}, 2, ~u64{0} - 4, 100) ==
                  DaveSenderError::none &&
              sender.next_expiry_ms == ~u64{0},
          "DAVE deadline addition saturates instead of wrapping");
    dave_receive_sender_release(&sender);
}
void many_epochs(Check check, KdfSha256Workspace& workspace) noexcept {
    DaveReceiveSender sender{};
    check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
          "DAVE many-epoch sender init");
    for (u32 i = 0; i != 12; ++i)
        transition(check, sender, i, i + 1, i, 100, Gcm64Backend::automatic);
    const auto result = open(check, sender, workspace, 0, 1, 20);
    check(sender.count == 12 && result.ok() && result.revision == 1 && result.epochs_tried == 12,
          "DAVE retained epochs do not inherit a reference implementation's unrelated eight "
          "staged-transition slot cap");
    check(dave_receive_sender_collect(sender, identity, 112) == DaveSenderError::none && sender.count == 1,
          "DAVE collection releases a batch of obsolete epochs");
    dave_receive_sender_release(&sender);
}
void guarded_retry(Check check, KdfSha256Workspace& workspace) noexcept {
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    u8* pages[3]{};
    bool ready = true;
    for (auto& page : pages) {
        page = static_cast<u8*>(
            VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD previous = 0;
        ready = ready && page &&
                VirtualProtect(page + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous);
    }
    check(ready, "DAVE sender immutable source/exact output/scratch guard pages");
    DaveReceiveSender sender{};
    if (ready) {
        check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
              "DAVE guarded sender init");
        transition(check, sender, 0, 1, 0, 100, Gcm64Backend::automatic);
        transition(check, sender, 1, 2, 1, 100, Gcm64Backend::automatic);
        auto* input = pages[0] + info.dwPageSize - 64;
        auto* output = pages[1] + info.dwPageSize - 48;
        auto* scratch = pages[2] + info.dwPageSize - 48;
        wire(check, 0, 1, input);
        u8 original[64];
        copy_bytes(original, input, 64);
        DWORD previous = 0;
        check(VirtualProtect(pages[0], info.dwPageSize, PAGE_READONLY, &previous) != 0,
              "DAVE sender source page is read-only");
        const auto result = dave_receive_sender_open(sender, identity, workspace, {input, 64}, 2,
                                                     {scratch, 48}, {output, 48});
        correct(check, result, 0, output);
        check(result.epochs_tried == 2 && bytes_equal(original, input, 64) && test_filled(scratch, 48, 0),
              "DAVE multi-epoch retry respects exact guarded buffers and immutable source");
    }
    dave_receive_sender_release(&sender);
    for (auto* page : pages)
        if (page)
            VirtualFree(page, 0, MEM_RELEASE);
}
void expired_transition(Check check, KdfSha256Workspace& workspace) noexcept {
    DaveReceiveSender sender{};
    check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
          "DAVE no-media transition sender init");
    transition(check, sender, 0, 1, 0, 1, Gcm64Backend::automatic);
    transition(check, sender, 1, 2, 1, 1, Gcm64Backend::automatic);
    // No collect call or frame arrives before the next transition. Elapsed
    // old epochs must be removed before refreshing grace for live epochs.
    transition(check, sender, 2, 3, 3, 10, Gcm64Backend::automatic);
    check(sender.count == 2 && sender.next_expiry_ms == 13,
          "DAVE transition itself collects expired nodes before renewing grace");
    check(!open(check, sender, workspace, 0, 1, 3).ok(),
          "DAVE transition cannot revive expired old key without media");
    const auto result = open(check, sender, workspace, 1, 1, 3);
    check(result.ok() && result.revision == 2,
          "DAVE live previous epoch receives new grace after idle transition");
    u8 plain[3]{0xf8, 0xff, 0xfe}, output[3];
    const auto rejected =
        dave_receive_sender_open(sender, identity, workspace, {plain, 3}, 3, {}, {output, 3});
    check(rejected.error == DaveSenderError::invalid_frame && !rejected.epochs_tried,
          "DAVE encrypted sender API never silently treats plaintext/silence as authenticated");
    dave_receive_sender_release(&sender);
}
} // namespace
void test_dave_receive_sender(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "DAVE sender shared KDF workspace");
    if (!workspace.storage.data)
        return;
    transitions(check, workspace, Gcm64Backend::automatic);
    transitions(check, workspace, Gcm64Backend::portable);
    boundaries(check, workspace);
    many_epochs(check, workspace);
    guarded_retry(check, workspace);
    expired_transition(check, workspace);
    kdf_sha256_release(&workspace);
}
} // namespace maicord

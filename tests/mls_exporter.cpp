#include "maicord/app/mls_exporter.hpp"
#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/core/memory.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_export_vectors.hpp"
void hashing(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 input[1025], expected[32], output[48];
    for (u32 i = 0; i != sizeof(input); ++i)
        input[i] = static_cast<u8>(i * 13 + 5);
    for (const auto& vector : sha_vectors) {
        test_unhex(vector.digest, expected);
        for (u32 split = 0; split <= vector.bytes; ++split) {
            const ConstSlice<u8> parts[]{{input, split}, {}, {input + split, vector.bytes - split}};
            memset(output, 0xa5, sizeof(output));
            check(sha256(workspace, {parts, 3}, {output, 48}).ok() && bytes_equal(output, expected, 32) &&
                      test_filled(output + 32, 16, 0xa5) &&
                      test_filled(workspace.storage.data, workspace.storage.capacity, 0),
                  "SHA-256 segmented padding boundaries match independent digest and preserve output tail");
        }
    }
    constexpr u8 abc[]{'a', 'b', 'c'};
    const ConstSlice<u8> parts[]{{abc, 3}};
    test_unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected);
    check(sha256(workspace, {parts, 1}, {output, 32}).ok() && bytes_equal(output, expected, 32),
          "SHA-256 abc known answer is distinct from HMAC");
}
void matrix(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 secret[32], labels[100], context[1025], output[8176];
    for (u32 i = 0; i != 32; ++i)
        secret[i] = static_cast<u8>(i);
    for (u32 i = 0; i != 100; ++i)
        labels[i] = static_cast<u8>(97 + i % 26);
    for (u32 i = 0; i != 1025; ++i)
        context[i] = static_cast<u8>(i * 13 + 5);
    constexpr u32 label_sizes[]{0, 1, 23, 24, 55, 56, 100};
    constexpr u32 context_sizes[]{0, 1, 8, 55, 56, 63, 64, 65, 1025};
    constexpr u32 sizes[]{0, 1, 16, 31, 32, 33, 65, 8160};
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS export matrix independent aggregation");
    if (!hash)
        return;
    for (auto label_size : label_sizes)
        for (auto context_size : context_sizes)
            for (auto size : sizes) {
                memset(output, 0xa5, sizeof(output));
                const auto result = mls_export_secret(workspace, {secret, 32}, {labels, label_size},
                                                      {context, context_size}, {output, size});
                check(result.ok() && result.bytes == size &&
                          test_filled(output + size, sizeof(output) - size, 0xa5),
                      "MLS exporter preserves exact output length across independent matrix");
                if (size)
                    check(BCryptHashData(hash, output, size, 0) >= 0, "MLS exporter aggregate update");
            }
    u8 actual[32], expected[32];
    test_unhex(exporter_matrix_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(actual, expected, 32),
          "MLS exporter 504 cases match independent OpenSSL results");
    BCryptDestroyHash(hash);
}
void receive(Check check, KdfSha256Workspace& workspace) noexcept {
    DaveReceiveSender sender{};
    for (const auto& vector : export_vectors) {
        const DaveReceiveIdentity identity{101, vector.user};
        if (!vector.epoch)
            check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
                  "MLS exporter sender init");
        u8 secret[32], expected[16], seed[32];
        for (u32 i = 0; i != 32; ++i)
            secret[i] = static_cast<u8>(i + vector.epoch * 19);
        test_unhex(vector.seed, expected);
        memset(seed, 0xa5, sizeof(seed));
        check(dave_export_media_seed(workspace, {secret, 32}, vector.user, {seed, 32}).ok() &&
                  bytes_equal(seed, expected, 16) && test_filled(seed + 16, 16, 0xa5),
              "DAVE export label/little-endian full-width user ID match independent seed");
        const u64 now = vector.epoch * 10;
        check(dave_receive_sender_transition_exporter(sender, identity, vector.epoch + 1, workspace,
                                                      {secret, 32}, now, 100)
                  .ok(),
              "DAVE receive transition exports the checked sender identity's seed");
        u8 frame[64], output[48];
        u32 size = test_unhex(vector.first, frame);
        memset(output, 0xa5, sizeof(output));
        const auto opened =
            dave_receive_sender_open(sender, identity, workspace, {frame, size}, now, {}, {output, 48});
        bool correct = opened.ok() && opened.revision == vector.epoch + 1 && opened.bytes == 32;
        for (u32 i = 0; i != 32 && correct; ++i)
            correct = output[i] == static_cast<u8>(i * 7 + vector.epoch * 9);
        check(correct && test_filled(output + 32, 16, 0xa5),
              "MLS exporter -> sender transition -> ratchet -> frame authentication/replay completes");
        if (vector.epoch) {
            // The preceding vector is this user's old epoch. Its second frame
            // has not been consumed, so it can arrive after the new epoch.
            const auto& old = *(&vector - 1);
            size = test_unhex(old.second, frame);
            const auto reordered =
                dave_receive_sender_open(sender, identity, workspace, {frame, size}, now, {}, {output, 48});
            check(reordered.ok() && reordered.revision == 1 && reordered.epochs_tried == 2,
                  "MLS exporter-derived old epoch remains usable during transition grace");
            dave_receive_sender_release(&sender);
        }
    }
}
void failures(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 secret[32]{}, output[48], label[8]{}, context[8]{};
    memset(output, 0xa5, sizeof(output));
    for (u32 size = 0; size != 16; ++size)
        check(dave_export_media_seed(workspace, {secret, 32}, 1, {output, size}).error ==
                      KdfError::output_capacity &&
                  test_filled(output, 48, 0xa5),
              "DAVE export short output is unchanged");
    check(dave_export_media_seed(workspace, {secret, 32}, 0, {output, 16}).error ==
                  KdfError::invalid_input &&
              mls_export_secret(workspace, {secret, 31}, {}, {}, {output, 16}).error ==
                  KdfError::invalid_input,
          "MLS exporter rejects invalid secret size and absent sender identity");
    check(mls_export_secret(workspace, {secret, 32}, {label, 8}, {nullptr, 1}, {output, 16}).error ==
                  KdfError::invalid_input &&
              test_filled(output, 48, 0xa5),
          "MLS exporter preflights invalid context before any derivation");
    check(mls_export_secret(workspace, {secret, 32}, {}, {}, {secret + 1, 16}).error ==
                  KdfError::invalid_input &&
              mls_export_secret(workspace, {secret, 32}, {}, {}, {output, 8161}).error ==
                  KdfError::invalid_input,
          "MLS exporter rejects overlap and oversized output without writes");
    check(mls_export_secret(workspace, {secret, 32}, {}, {}, {workspace.storage.data, 16}).error ==
              KdfError::invalid_input,
          "MLS exporter protects the reusable workspace");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    const auto failed = mls_export_secret(workspace, {secret, 32}, {label, 8}, {context, 8}, {output, 16});
    workspace.storage.capacity = capacity;
    check(failed.error == KdfError::platform_failure && !failed.bytes && test_filled(output, 16, 0) &&
              test_filled(output + 16, 32, 0xa5),
          "MLS export provider failure erases only requested output");
    DaveReceiveSender sender{};
    const DaveReceiveIdentity identity{10, 20};
    check(dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none,
          "MLS exporter failure sender init");
    check(
        dave_receive_sender_transition_exporter(sender, identity, 1, workspace, {secret, 32}, 0, 100).ok(),
        "MLS exporter failure baseline epoch");
    workspace.storage.capacity = 1;
    check(dave_receive_sender_transition_exporter(sender, {10, 21}, 2, workspace, {nullptr, 32}, 1, 100)
                      .error == DaveSenderError::identity_mismatch &&
              dave_receive_sender_transition_exporter(sender, identity, 1, workspace, {nullptr, 32}, 1, 100)
                      .error == DaveSenderError::stale_transition,
          "DAVE exporter transition rejects wrong identity/stale command before hashing");
    const auto replacement =
        dave_receive_sender_transition_exporter(sender, identity, 2, workspace, {secret, 32}, 10, 100);
    workspace.storage.capacity = capacity;
    check(replacement.error == DaveSenderError::epoch_export && !replacement.epoch.bytes &&
              sender.count == 1 && sender.latest_revision == 1 && sender.last_now_ms == 0 &&
              sender.next_expiry_ms == ~u64{0},
          "DAVE failed export preserves existing key epochs, replay lifetime and transition revision");
    check(
        dave_receive_sender_transition_exporter(sender, identity, 2, workspace, {secret, 32}, 10, 100).ok(),
        "DAVE exporter transition can retry after provider failure");
    dave_receive_sender_release(&sender);
}
void guards(Check check, KdfSha256Workspace& workspace) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u32 usable = ((8160 + info.dwPageSize - 1) / info.dwPageSize) * info.dwPageSize;
    auto* input_pages = static_cast<u8*>(
        VirtualAlloc(nullptr, usable + info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* output_pages = static_cast<u8*>(
        VirtualAlloc(nullptr, usable + info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD previous = 0;
    const bool ready = input_pages && output_pages &&
                       VirtualProtect(input_pages + usable, info.dwPageSize, PAGE_NOACCESS, &previous) &&
                       VirtualProtect(output_pages + usable, info.dwPageSize, PAGE_NOACCESS, &previous);
    check(ready, "MLS exporter guard pages");
    if (ready) {
        u8 secret[32]{}, expected[8160];
        constexpr u32 lengths[]{0, 1, 55, 56, 63, 64, 65, 1025};
        constexpr u32 sizes[]{16, 32, 33, 65, 8160};
        for (u32 length : lengths) {
            auto* input = input_pages + usable - length;
            for (u32 i = 0; i != length; ++i)
                input[i] = static_cast<u8>(i * 13 + 5);
            check(VirtualProtect(input_pages, usable, PAGE_READONLY, &previous) != 0,
                  "MLS exporter input is immutable through hash/label expansion");
            const ConstSlice<u8> parts[]{{input, length / 2}, {input + length / 2, length - length / 2}};
            check(sha256(workspace, {parts, 2}, {output_pages + usable - 32, 32}).ok(),
                  "SHA-256 exact input/output guarded boundaries");
            for (u32 size : sizes) {
                const auto normal = mls_export_secret(workspace, {secret, 32}, {input, length},
                                                      {input, length}, {expected, size});
                const auto guarded =
                    mls_export_secret(workspace, {secret, 32}, {input, length}, {input, length},
                                      {output_pages + usable - size, size});
                check(normal.ok() && guarded.ok() &&
                          bytes_equal(expected, output_pages + usable - size, size),
                      "MLS exporter maximum/partial output blocks respect exact protected capacity");
            }
            const bool writable = VirtualProtect(input_pages, usable, PAGE_READWRITE, &previous) != 0;
            check(writable, "MLS exporter guard input restore");
            if (!writable)
                break;
        }
    }
    if (input_pages)
        VirtualFree(input_pages, 0, MEM_RELEASE);
    if (output_pages)
        VirtualFree(output_pages, 0, MEM_RELEASE);
}
} // namespace
void test_mls_exporter(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "MLS exporter shared workspace init");
    if (!workspace.storage.data)
        return;
    hashing(check, workspace);
    matrix(check, workspace);
    receive(check, workspace);
    failures(check, workspace);
    guards(check, workspace);
    kdf_sha256_release(&workspace);
}
} // namespace maicord

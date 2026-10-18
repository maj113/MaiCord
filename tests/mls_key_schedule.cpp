#include "maicord/app/mls_key_schedule.hpp"
#include "maicord/app/mls_exporter.hpp"
#include "maicord/app/hpke_derivation.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_schedule_vectors.hpp"
void published(Check check, KdfSha256Workspace& workspace) noexcept {
    MlsEpochSecrets current{}, expected, joined;
    bool first = true;
    for (const auto& vector : schedule_vectors) {
        u8 init[32], commit[32], psk[32], confirmed[32], context[256];
        test_unhex(vector.init, init);
        test_unhex(vector.commit, commit);
        test_unhex(vector.psk, psk);
        test_unhex(vector.confirmed, confirmed);
        const u32 context_size = test_unhex(vector.context, context);
        check(test_unhex(vector.expected, reinterpret_cast<u8*>(&expected)) == sizeof(expected),
              "MLS schedule fixture contains every candidate secret/tag");
        if (!first)
            check(bytes_equal(init, current.init_secret, 32),
                  "MLS published epoch init chains from predecessor");
        const auto result =
            mls_epoch_next(workspace, {first ? init : current.init_secret, 32}, {commit, 32}, {psk, 32},
                           {confirmed, 32}, {context, context_size}, &current);
        first = false;
        check(result.ok() && result.bytes == sizeof(current) &&
                  bytes_equal(&current, &expected, sizeof(current)),
              "MLS ciphersuite-2 published schedule matches all secrets, welcome and confirmation tag");
        check(mls_epoch_from_joiner(workspace, {current.joiner_secret, 32}, {psk, 32}, {confirmed, 32},
                                    {context, context_size}, &joined)
                      .ok() &&
                  bytes_equal(&joined, &current, sizeof(current)),
              "MLS Welcome joiner path derives identical candidate material");
        u8 label[128], export_context[128], exported[64], answer[64];
        const u32 label_size = test_unhex(vector.label, label),
                  context_bytes = test_unhex(vector.export_context, export_context);
        const u32 length = test_unhex(vector.exported, answer);
        check(mls_export_secret(workspace, {current.exporter_secret, 32}, {label, label_size},
                                {export_context, context_bytes}, {exported, length})
                      .ok() &&
                  bytes_equal(exported, answer, length),
              "MLS key schedule's exporter agrees with published working-group exporter output");
        check(mls_confirmation_matches({current.confirmation_tag, 32}, {expected.confirmation_tag, 32}),
              "MLS complete confirmation tag matches");
        P256Key external{};
        u8 external_point[65], expected_point[65];
        test_unhex(vector.external_pub, expected_point);
        check(hpke_derive_key_pair(workspace, {current.external_secret, 32}, &external).ok() &&
                  p256_export_public(external, {external_point, 65}).ok() &&
                  bytes_equal(external_point, expected_point, 65),
              "MLS chained epoch schedule derives published external HPKE public key");
        p256_release(&external);
        for (u32 bit = 0; bit != 256; ++bit) {
            expected.confirmation_tag[bit / 8] ^= static_cast<u8>(1u << (bit % 8));
            check(
                !mls_confirmation_matches({current.confirmation_tag, 32}, {expected.confirmation_tag, 32}),
                "MLS confirmation rejects every single-bit tamper");
            expected.confirmation_tag[bit / 8] ^= static_cast<u8>(1u << (bit % 8));
        }
    }
    mls_epoch_clear(&current);
    mls_epoch_clear(&expected);
    mls_epoch_clear(&joined);
    check(test_filled(reinterpret_cast<const u8*>(&current), sizeof(current), 0),
          "MLS epoch retirement erases all material");
}
void initial_and_transcripts(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 init[32];
    for (u32 i = 0; i != 32; ++i)
        init[i] = static_cast<u8>(i);
    constexpr char context[] = "initial synthetic context", content[] = "validated confirmed input";
    MlsEpochSecrets epoch, expected;
    test_unhex(initial_schedule, reinterpret_cast<u8*>(&expected));
    check(mls_epoch_initial(workspace, {init, 32},
                            {reinterpret_cast<const u8*>(context), sizeof(context) - 1}, &epoch)
                  .ok() &&
              bytes_equal(&epoch, &expected, sizeof(epoch)),
          "MLS initial epoch uses empty confirmed hash and zero commit/PSK inputs");
    u8 interim[48], confirmed[48], next[48], answer[32];
    memset(interim, 0xa5, sizeof(interim));
    memset(confirmed, 0xa5, sizeof(confirmed));
    memset(next, 0xa5, sizeof(next));
    test_unhex(initial_interim, answer);
    check(mls_transcript_interim(workspace, {}, {epoch.confirmation_tag, 32}, {interim, 48}).ok() &&
              bytes_equal(interim, answer, 32),
          "MLS initial interim transcript includes the serialized confirmation-tag length");
    test_unhex(next_confirmed, answer);
    const ConstSlice<u8> confirmed_parts[]{{interim, 32},
                                           {reinterpret_cast<const u8*>(content), sizeof(content) - 1}};
    check(sha256(workspace, {confirmed_parts, 2}, {confirmed, 48})
                  .ok() &&
              bytes_equal(confirmed, answer, 32),
          "MLS confirmed transcript hashes previous interim and serialized content input");
    test_unhex(next_interim, answer);
    check(
        mls_transcript_interim(workspace, {confirmed, 32}, {epoch.confirmation_tag, 32}, {next, 48}).ok() &&
            bytes_equal(next, answer, 32) && test_filled(interim + 32, 16, 0xa5) &&
            test_filled(confirmed + 32, 16, 0xa5) && test_filled(next + 32, 16, 0xa5),
        "MLS subsequent interim transcript and output tails match independent vectors");
    mls_epoch_clear(&epoch);
    mls_epoch_clear(&expected);
}
void matrix(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 init[32], zero[32]{}, context[16384], actual[32], expected[32];
    for (u32 i = 0; i != 32; ++i)
        init[i] = static_cast<u8>(i);
    for (u32 i = 0; i != sizeof(context); ++i)
        context[i] = static_cast<u8>(i * 17 + 3);
    constexpr u32 lengths[]{0, 1, 55, 56, 63, 64, 65, 16383, 16384};
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS schedule digest init");
    if (!hash)
        return;
    MlsEpochSecrets epoch;
    for (u32 length : lengths) {
        for (u32 mode = 0; mode != 3; ++mode) {
            const auto result = mode == 0
                                    ? mls_epoch_initial(workspace, {init, 32}, {context, length}, &epoch)
                                : mode == 1 ? mls_epoch_next(workspace, {init, 32}, {init, 32}, {zero, 32},
                                                             {init, 32}, {context, length}, &epoch)
                                            : mls_epoch_from_joiner(workspace, {init, 32}, {init, 32},
                                                                    {init, 32}, {context, length}, &epoch);
            check(result.ok() && BCryptHashData(hash, reinterpret_cast<u8*>(&epoch), sizeof(epoch), 0) >= 0,
                  "MLS schedule context-length boundary derivation");
        }
    }
    test_unhex(schedule_matrix_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(actual, expected, 32),
          "MLS initial/next/joiner context boundaries match independent aggregate");
    BCryptDestroyHash(hash);
    mls_epoch_clear(&epoch);
}
void failures(Check check, KdfSha256Workspace& workspace) noexcept {
    MlsEpochSecrets output;
    memset(&output, 0xa5, sizeof(output));
    u8 input[32]{}, tag[32]{};
    check(mls_epoch_next(workspace, {input, 32}, {input, 32}, {input, 32}, {}, {}, &output).error ==
                  KdfError::invalid_input &&
              mls_epoch_initial(workspace, {input, 31}, {}, &output).error == KdfError::invalid_input &&
              mls_epoch_initial(workspace, {input, 32}, {nullptr, 1}, &output).error ==
                  KdfError::invalid_input &&
              test_filled(reinterpret_cast<const u8*>(&output), sizeof(output), 0xa5),
          "MLS schedule validates all inputs before changing previous candidate");
    check(mls_epoch_initial(workspace, {input, 32}, {},
                            reinterpret_cast<MlsEpochSecrets*>(workspace.storage.data))
                  .error == KdfError::invalid_input,
          "MLS schedule protects worker hash workspace");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(mls_epoch_initial(workspace, {input, 32}, {input, 0x40000000}, &output).error ==
              KdfError::invalid_input,
          "MLS context vector overflow is rejected before provider work");
    const auto failed = mls_epoch_initial(workspace, {output.init_secret, 32}, {}, &output);
    workspace.storage.capacity = capacity;
    check(failed.error == KdfError::platform_failure && !failed.bytes &&
              test_filled(reinterpret_cast<const u8*>(&output), sizeof(output), 0xa5),
          "MLS provider failure preserves in-place predecessor and all secrets");
    check(mls_epoch_initial(workspace, {input, 32}, {}, &output).ok(),
          "MLS schedule retries after provider failure");
    for (u32 size = 0; size != 32; ++size)
        check(!mls_confirmation_matches({tag, 32}, {tag, size}) &&
                  mls_transcript_interim(workspace, {}, {tag, 32}, {input, size}).error ==
                      KdfError::output_capacity,
              "MLS confirmation never accepts a truncated tag and transcript never truncates output");
    mls_epoch_clear(&output);
}
void guards(Check check, KdfSha256Workspace& workspace) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* input_pages = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* output_pages = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old = 0;
    const bool ready =
        input_pages && output_pages &&
        VirtualProtect(input_pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old) &&
        VirtualProtect(output_pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old);
    check(ready, "MLS key schedule exact input/output guard pages");
    if (ready) {
        auto* output =
            reinterpret_cast<MlsEpochSecrets*>(output_pages + info.dwPageSize - sizeof(MlsEpochSecrets));
        u8 secret[32]{};
        MlsEpochSecrets expected;
        constexpr u32 lengths[]{0, 1, 63, 64, 65};
        for (u32 length : lengths) {
            auto* context = input_pages + info.dwPageSize - length;
            for (u32 i = 0; i != length; ++i)
                context[i] = static_cast<u8>(i);
            check(VirtualProtect(input_pages, info.dwPageSize, PAGE_READONLY, &old) != 0,
                  "MLS schedule context remains immutable through all derivations");
            check(mls_epoch_initial(workspace, {secret, 32}, {context, length}, &expected).ok() &&
                      mls_epoch_initial(workspace, {secret, 32}, {context, length}, output).ok() &&
                      bytes_equal(&expected, output, sizeof(expected)),
                  "MLS schedule accesses exact guarded context/output boundaries");
            mls_epoch_clear(output);
            check(test_filled(reinterpret_cast<const u8*>(output), sizeof(*output), 0),
                  "MLS schedule clears exactly its guarded candidate allocation");
            const bool restored = VirtualProtect(input_pages, info.dwPageSize, PAGE_READWRITE, &old) != 0;
            check(restored, "MLS schedule guard context restore");
            if (!restored)
                break;
        }
        mls_epoch_clear(&expected);
    }
    if (input_pages)
        VirtualFree(input_pages, 0, MEM_RELEASE);
    if (output_pages)
        VirtualFree(output_pages, 0, MEM_RELEASE);
}
} // namespace
void test_mls_key_schedule(Check check) noexcept {
    static_assert(sizeof(MlsEpochSecrets) == 13 * 32);
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "MLS key schedule workspace init");
    if (!workspace.storage.data)
        return;
    published(check, workspace);
    initial_and_transcripts(check, workspace);
    matrix(check, workspace);
    failures(check, workspace);
    guards(check, workspace);
    kdf_sha256_release(&workspace);
}
} // namespace maicord

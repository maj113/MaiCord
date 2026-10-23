#include "maicord/app/mls_key_package.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_package_policy_vectors.hpp"
bool filled(const void* value, u32 count) noexcept {
    return test_filled(static_cast<const u8*>(value), count, 0xa5);
}
void guard(Check check, KdfSha256Workspace& workspace, ConstSlice<u8> input) noexcept {
    Win32SystemInfo system{};
    GetSystemInfo(system.native());
    const u32 page = system.dwPageSize;
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(memory != nullptr && input.count < page, "DAVE profile guard allocation");
    if (!memory)
        return;
    auto* source = memory + page - input.count;
    copy_bytes(source, input.data, input.count);
    auto* output = reinterpret_cast<MlsSignedKeyPackage*>(memory + page * 3 - sizeof(MlsSignedKeyPackage));
    DWORD old = 0;
    check(VirtualProtect(memory, page, PAGE_READONLY, &old) &&
              VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &old),
          "DAVE profile immutable input and exact output guards");
    check(mls_key_package_check_dave(workspace, {source, input.count}, 42, output).ok() &&
              output->view.serialized.data == source,
          "DAVE profile/crypto checks preserve borrowed input at guard boundaries");
    memset(output, 0xa5, sizeof(*output));
    check(mls_key_package_check_dave(workspace, {source, input.count}, 43, output).error ==
                  MlsPackageCryptoError::identity_mismatch &&
              filled(output, sizeof(*output)),
          "DAVE guarded mismatch preserves full output");
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_dave_package_policy(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "DAVE policy workspace");
    auto* buffer = static_cast<u8*>(VirtualAlloc(nullptr, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffer != nullptr, "DAVE policy fixture buffer");
    if (!buffer) {
        kdf_sha256_release(&workspace);
        return;
    }
    for (const auto& fixture : dave_policy_vectors) {
        const u32 count = test_unhex(fixture.wire, buffer);
        MlsSignedKeyPackage crypto{};
        check(mls_key_package_check_crypto(workspace, {buffer, count}, &crypto).ok(),
              "Every independent DAVE policy fixture has valid crypto, including rejected profiles");
        MlsSignedKeyPackage checked;
        memset(&checked, 0xa5, sizeof(checked));
        const auto result = mls_key_package_check_dave(workspace, {buffer, count}, 42, &checked);
        check(result.error == fixture.error, fixture.name);
        if (fixture.error == MlsPackageCryptoError::none) {
            check(result.ok() && result.bytes == count && checked.view.serialized.data == buffer &&
                      bytes_equal(checked.reference, crypto.reference, 32),
                  "DAVE accepted profile retains exact signed wire and reference");
            buffer[count - 1] ^= 1;
            memset(&checked, 0xa5, sizeof(checked));
            check(mls_key_package_check_dave(workspace, {buffer, count}, 42, &checked).error ==
                          MlsPackageCryptoError::invalid_signature &&
                      filled(&checked, sizeof(checked)),
                  "A valid DAVE profile never bypasses signature verification");
            buffer[count - 1] ^= 1;
        } else {
            check(!result.bytes && filled(&checked, sizeof(checked)),
                  "DAVE rejected profile preserves previous output");
            const u32 capacity = workspace.storage.capacity;
            workspace.storage.capacity = 1; // A crypto operation would fail with hash_failure.
            const auto early = mls_key_package_check_dave(workspace, {buffer, count}, 42, &checked);
            workspace.storage.capacity = capacity;
            check(early.error == fixture.error && !early.native_status && filled(&checked, sizeof(checked)),
                  "DAVE profile rejection precedes crypto provider work");
        }
    }
    const u32 count = test_unhex(dave_policy_vectors[0].wire, buffer);
    MlsSignedKeyPackage unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    check(mls_key_package_check_dave(workspace, {buffer, count}, 0, &unchanged).error ==
                  MlsPackageCryptoError::invalid_input &&
              filled(&unchanged, sizeof(unchanged)),
          "DAVE missing expected identity cannot select crypto-only behavior");
    check(mls_key_package_check_dave(workspace, {buffer, count}, 43, &unchanged).error ==
                  MlsPackageCryptoError::identity_mismatch &&
              filled(&unchanged, sizeof(unchanged)),
          "DAVE expected identity is external, not inferred from the signed credential");
    check(mls_key_package_check_dave(workspace, {buffer, count}, 42,
                                     reinterpret_cast<MlsSignedKeyPackage*>(buffer))
                  .error == MlsPackageCryptoError::invalid_input,
          "DAVE profile API rejects output overlapping signed input");
    check(mls_key_package_check_dave(workspace, {workspace.storage.data, count}, 42, &unchanged).error ==
                  MlsPackageCryptoError::invalid_input &&
              filled(&unchanged, sizeof(unchanged)),
          "DAVE profile API rejects erased workspace input");
    check(mls_key_package_check_dave(workspace, {buffer, count}, 42,
                                     reinterpret_cast<MlsSignedKeyPackage*>(&workspace))
                  .error == MlsPackageCryptoError::invalid_input,
          "DAVE profile API rejects workspace owner output");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(mls_key_package_check_dave(workspace, {buffer, count}, 42, &unchanged).error ==
                  MlsPackageCryptoError::hash_failure &&
              filled(&unchanged, sizeof(unchanged)),
          "DAVE accepted profile still propagates provider failure atomically");
    workspace.storage.capacity = capacity;
    guard(check, workspace, {buffer, count});
    VirtualFree(buffer, 0, MEM_RELEASE);
    kdf_sha256_release(&workspace);
}
} // namespace maicord

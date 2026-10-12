#include "maicord/app/dave_key_package.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
bool zero(const void* value, u32 count) noexcept {
    const auto* b = static_cast<const u8*>(value);
    for (u32 i = 0; i != count; ++i)
        if (b[i])
            return false;
    return true;
}
void record(Check check, HANDLE file, const DaveKeyPackage& package) noexcept {
    if (file == INVALID_HANDLE_VALUE)
        return;
    u8 header[12];
    for (u32 i = 0; i != 8; ++i)
        header[7 - i] = static_cast<u8>(package.user_id >> (8 * i));
    for (u32 i = 0; i != 4; ++i)
        header[11 - i] = static_cast<u8>(package.bytes >> (8 * i));
    const ConstSlice<u8> parts[]{{header, 12}, {package.wire, package.bytes}, {package.reference, 32}};
    for (auto part : parts) {
        DWORD written = 0;
        check(WriteFile(file, part.data, part.count, &written, nullptr) && written == part.count,
              "DAVE public KeyPackage oracle record");
    }
}
void retained_key(Check check, const P256Key& private_key, ConstSlice<u8> point) noexcept {
    P256Key peer{}, public_key{};
    check(p256_generate(&peer, P256Kind::ecdh).ok() &&
              p256_import_public(&public_key, P256Kind::ecdh, point).ok(),
          "DAVE package retained-key verification setup");
    u8 a[32], b[32];
    check(p256_ecdh(private_key, peer, {a, 32}).ok() && p256_ecdh(peer, public_key, {b, 32}).ok() &&
              bytes_equal(a, b, 32),
          "DAVE retained private key agrees with serialized public key");
    SecureZeroMemory(a, sizeof(a));
    SecureZeroMemory(b, sizeof(b));
    p256_release(&peer);
    p256_release(&public_key);
}
void guards(Check check, KdfSha256Workspace& workspace, ConstSlice<u8> scalar) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(memory != nullptr, "DAVE package guard allocation");
    if (!memory)
        return;
    auto* signing = reinterpret_cast<P256Key*>(memory + page - sizeof(P256Key));
    auto* package = reinterpret_cast<DaveKeyPackage*>(memory + page * 3 - sizeof(DaveKeyPackage));
    check(p256_import_private(signing, P256Kind::ecdsa, scalar).ok(), "DAVE guard signing key");
    DWORD old = 0;
    check(VirtualProtect(memory, page, PAGE_READONLY, &old) &&
              VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &old),
          "DAVE immutable signing owner and exact output guard");
    check(dave_key_package_create(workspace, *signing, 123, package).ok(),
          "DAVE signed package with guarded output and immutable signing owner");
    MlsSignedKeyPackage verified{};
    check(mls_key_package_check_dave(workspace, {package->wire, package->bytes}, 123, &verified).ok(),
          "DAVE guarded signed output verifies");
    dave_key_package_release(package);
    check(zero(package, sizeof(*package)), "DAVE guarded release erases exactly owner");
    check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "DAVE guard signing cleanup access");
    p256_release(signing);
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_dave_key_package(Check check) noexcept {
    P256Key signing{};
    u8 scalar[32]{};
    scalar[31] = 123; // Public QA key, never a real account key.
    check(p256_import_private(&signing, P256Kind::ecdsa, {scalar, 32}).ok(),
          "DAVE package signing fixture");
    KdfSha256Workspace workspace{};
    DaveKeyPackage output{};
    check(dave_key_package_create(workspace, signing, 1, &output).error ==
                  DavePackageError::not_initialized &&
              zero(&output, sizeof(output)),
          "DAVE package requires worker workspace");
    check(kdf_sha256_initialize(&workspace).ok(), "DAVE package workspace");
    wchar_t path[1024];
    const DWORD path_bytes = GetEnvironmentVariableW(L"MAICORD_DAVE_PACKAGE_OUTPUT", path, 1024);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (path_bytes) {
        check(path_bytes < 1024, "DAVE package oracle path length");
        if (path_bytes < 1024)
            file =
                CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "DAVE package oracle output file");
    }
    const u64 users[]{1, 255, 256, 0x0102030405060708ull, 0x8000000000000000ull, ~u64{0}, 42, 42};
    u8 previous_init[65]{}, previous_leaf[65]{};
    for (const auto user : users) {
        const auto created = dave_key_package_create(workspace, signing, user, &output);
        check(created.ok() && created.bytes == output.bytes && output.bytes <= dave_key_package_capacity &&
                  output.user_id == user,
              "DAVE signed package creation");
        if (!created.ok())
            break;
        MlsSignedKeyPackage checked{};
        const auto verified =
            mls_key_package_check_dave(workspace, {output.wire, output.bytes}, user, &checked);
        check(verified.ok() && bytes_equal(checked.reference, output.reference, 32),
              "DAVE native package verifies both signatures and complete reference");
        if (!verified.ok()) {
            dave_key_package_release(&output);
            break;
        }
        const auto& leaf = checked.view.leaf;
        check(leaf.credential.type == 1 && leaf.credential.identity.count == 8 && leaf.not_before == 0 &&
                  leaf.not_after == ~u64{0} && !leaf.extensions.count && !checked.view.extensions.count,
              "DAVE mandated credential and lifetime profile");
        u64 actual_user = 0;
        for (u32 i = 0; i != leaf.credential.identity.count; ++i)
            actual_user = (actual_user << 8) | leaf.credential.identity[i];
        check(actual_user == user, "DAVE identity is full big-endian user ID");
        check(!bytes_equal(previous_init, checked.view.init_key.data, 65) &&
                  !bytes_equal(previous_leaf, leaf.encryption_key.data, 65),
              "DAVE package replacement generates fresh init and leaf points");
        copy_bytes(previous_init, checked.view.init_key.data, 65);
        copy_bytes(previous_leaf, leaf.encryption_key.data, 65);
        retained_key(check, output.init_key, checked.view.init_key);
        retained_key(check, output.leaf_key, leaf.encryption_key);
        record(check, file, output);
        u8 snapshot[sizeof(output)];
        copy_bytes(snapshot, &output, sizeof(output));
        check(dave_key_package_create(workspace, signing, user, &output).error ==
                      DavePackageError::already_initialized &&
                  bytes_equal(snapshot, &output, sizeof(output)),
              "DAVE cannot overwrite active pending package and private-key owners");
        dave_key_package_release(&output);
        check(zero(&output, sizeof(output)), "DAVE package release erases full owner and bytes");
        dave_key_package_release(&output);
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    check(dave_key_package_create(workspace, signing, 0, &output).error == DavePackageError::invalid_input,
          "DAVE rejects absent user identity");
    P256Key empty{};
    check(dave_key_package_create(workspace, empty, 1, &output).error == DavePackageError::invalid_input,
          "DAVE rejects missing signing key");
    u8 point[65];
    check(p256_export_public(signing, {point, 65}).ok(), "DAVE signing public export");
    P256Key public_only{}, wrong_kind{};
    check(p256_import_public(&public_only, P256Kind::ecdsa, {point, 65}).ok() &&
              p256_generate(&wrong_kind, P256Kind::ecdh).ok(),
          "DAVE signing preflight fixtures");
    check(dave_key_package_create(workspace, public_only, 1, &output).error ==
                  DavePackageError::invalid_input &&
              dave_key_package_create(workspace, wrong_kind, 1, &output).error ==
                  DavePackageError::invalid_input &&
              zero(&output, sizeof(output)),
          "DAVE rejects public-only and wrong-purpose signing keys atomically");
    p256_release(&public_only);
    p256_release(&wrong_kind);
    check(
        dave_key_package_create(workspace, signing, 1, reinterpret_cast<DaveKeyPackage*>(&signing)).error ==
                DavePackageError::invalid_input &&
            signing.handle,
        "DAVE output cannot overwrite signing owner");
    check(dave_key_package_create(workspace, signing, 1, reinterpret_cast<DaveKeyPackage*>(&workspace))
                  .error == DavePackageError::invalid_input,
          "DAVE output cannot overwrite workspace owner");
    check(dave_key_package_create(workspace, signing, 1,
                                  reinterpret_cast<DaveKeyPackage*>(workspace.storage.data))
                  .error == DavePackageError::invalid_input,
          "DAVE output cannot overwrite workspace storage");
    check(
        dave_key_package_create(workspace, *reinterpret_cast<P256Key*>(workspace.storage.data), 1, &output)
                    .error == DavePackageError::invalid_input &&
            zero(&output, sizeof(output)),
        "DAVE signing owner cannot alias erased workspace storage");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    for (u32 i = 0; i != 8; ++i)
        check(dave_key_package_create(workspace, signing, 1, &output).error ==
                      DavePackageError::hash_failure &&
                  zero(&output, sizeof(output)),
              "DAVE provider failure after key generation preserves output");
    workspace.storage.capacity = capacity;
    check(dave_key_package_create(workspace, signing, 1, &output).ok(),
          "DAVE retry after provider failure");
    dave_key_package_release(&output);
    guards(check, workspace, {scalar, 32});
    check(zero(workspace.storage.data, workspace.storage.capacity), "DAVE signing workspace erased");
    kdf_sha256_release(&workspace);
    p256_release(&signing);
    dave_key_package_release(nullptr);
}
} // namespace maicord

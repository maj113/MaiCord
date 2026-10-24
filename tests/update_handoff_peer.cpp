// Test-only freestanding process fixture. It is never packaged or shipped.
#include "maicord/app/update_handoff.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/account_host.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace {
maicord::u32 length(const wchar_t* s) noexcept { maicord::u32 n = 0; while (s[n] && n < maicord::kUpdateHandoffCommandCharacters) ++n; return n; }
int hex(wchar_t c) noexcept { if (c >= L'0' && c <= L'9') return c - L'0'; if (c >= L'a' && c <= L'f') return c - L'a' + 10; return -1; }
void fixture_event_name(const maicord::UpdateHandoffIdentity& id, wchar_t* name) noexcept {
    constexpr wchar_t prefix[] = L"Local\\MaiCord-Handoff-Fixture-";
    constexpr wchar_t digits[] = L"0123456789abcdef"; maicord::u32 n = 0;
    for (maicord::u32 i = 0; i < sizeof(prefix) / sizeof(wchar_t) - 1; ++i) name[n++] = prefix[i];
    for (auto byte : id.installation.bytes) { name[n++] = digits[byte >> 4]; name[n++] = digits[byte & 15]; }
    name[n] = 0;
}
bool fixture_ids(maicord::ConstSlice<wchar_t> line, maicord::UpdateHandoffIdentity* id) noexcept {
    // This peer deliberately obtains fixture IDs from its generated protocol;
    // production must independently obtain them from validated enrollment/state.
    maicord::u32 at = 0;
    if (line.data[at++] != L'"') return false;
    while (at < line.count && line.data[at] != L'"') ++at;
    constexpr wchar_t flag[] = L" --maicord-update-child-v1 ";
    if (at == line.count) return false; ++at;
    for (maicord::u32 i = 0; i < sizeof(flag) / sizeof(wchar_t) - 1; ++i) if (at >= line.count || line.data[at++] != flag[i]) return false;
    maicord::u8* outputs[]{id->installation.bytes, id->transaction.bytes, id->document_sha256};
    constexpr maicord::u32 sizes[]{16, 16, 32};
    for (maicord::u32 field = 0; field < 3; ++field) {
        for (maicord::u32 i = 0; i < sizes[field]; ++i) { if (at + 2 > line.count) return false;
            const int a = hex(line.data[at++]), b = hex(line.data[at++]); if (a < 0 || b < 0) return false;
            outputs[field][i] = static_cast<maicord::u8>((a << 4) | b); }
        if (at >= line.count || line.data[at++] != L' ') return false;
    }
    return true;
}
struct SignedPeerValidation {
    HANDLE journal{}, files[maicord::kUpdateGenerationMaximumFiles]{}; // generation files by slot
    maicord::KdfSha256Workspace workspace{};
    maicord::P256Key key{};
    maicord::UpdatePackageResult verification{};
    bool release() noexcept {
        bool ok = maicord::update_package_cleanup_release(&verification).ok();
        if (journal) { if (!CloseHandle(journal)) ok = false; else journal = nullptr; }
        for (auto& h : files) if (h) { if (!CloseHandle(h)) ok = false; else h = nullptr; }
        maicord::p256_release(&key); maicord::kdf_sha256_release(&workspace); return ok;
    }
};
bool signed_file(const wchar_t* path, HANDLE* output, maicord::u64* bytes) noexcept {
    *output = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*output == INVALID_HANDLE_VALUE) { *output = nullptr; return false; }
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(*output, &info) || !GetFileSizeEx(*output, &size) || size.QuadPart < 0 ||
        info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) return false;
    *bytes = static_cast<maicord::u64>(size.QuadPart); return true;
}
bool signed_read(HANDLE h, void* out, maicord::u32 bytes) noexcept {
    maicord::u32 done = 0;
    while (done < bytes) { DWORD got = 0; if (!ReadFile(h, static_cast<maicord::u8*>(out) + done, bytes - done, &got, nullptr) || !got || got > bytes - done) return false; done += got; }
    return true;
}
bool independently_validate_signed_fixture(SignedPeerValidation& owner, maicord::UpdateHandoffIdentity* identity) noexcept {
    using namespace maicord;
    // Fixture-only trusted public inventory. This is not production enrollment
    // or a seed dispatcher: no offer/command-line key or root is accepted.
    wchar_t module[kUpdateGenerationMaximumRootCharacters + 100]{}, generation[kUpdateGenerationMaximumRootCharacters + 100]{}, root[kUpdateGenerationMaximumRootCharacters + 1]{};
    const DWORD n = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(sizeof(module) / sizeof(wchar_t)));
    if (!n || n >= sizeof(module) / sizeof(wchar_t)) return false;
    u32 exe_separator = n; while (exe_separator && module[exe_separator - 1] != L'\\') --exe_separator;
    constexpr wchar_t executable[] = L"MaiCord.exe";
    if (!exe_separator || n - exe_separator != sizeof(executable) / sizeof(wchar_t) - 1) return false;
    for (u32 i = 0; i < sizeof(executable) / sizeof(wchar_t); ++i) if (module[exe_separator + i] != executable[i]) return false;
    u32 root_separator = exe_separator - 1; while (root_separator && module[root_separator - 1] != L'\\') --root_separator;
    if (root_separator < 4 || root_separator - 1 > kUpdateGenerationMaximumRootCharacters || exe_separator - 1 - root_separator != 66 ||
        module[root_separator] != L'g' || module[root_separator + 1] != L'-') return false;
    copy_bytes(root, module, (root_separator - 1) * sizeof(wchar_t));
    copy_bytes(generation, module, (exe_separator - 1) * sizeof(wchar_t));
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; signed_update_fixture::child(root, L"install.state", path);
    u64 bytes = 0; u8 record[kUpdateGenerationRecordBytes]{}; UpdateGenerationState state{};
    if (!signed_file(path, &owner.journal, &bytes) || bytes != sizeof(record) || !signed_read(owner.journal, record, sizeof(record)) ||
        !update_generation_decode({record,sizeof(record)}, &state).ok() || state.phase != UpdateGenerationPhase::pending || !state.active.present) return false;
    UpdateGenerationIdentity enrolled_fixture{}; enrolled_fixture.bytes[0] = 0x53;
    UpdateGenerationIdentity expected_transaction{}; expected_transaction.bytes[0] = 0xe1;
    if (!bytes_equal(state.installation.bytes, enrolled_fixture.bytes, 16) || !bytes_equal(state.transaction.bytes, expected_transaction.bytes, 16)) return false;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) if (module[root_separator + 2 + i * 2] != digits[state.active.package.document_sha256[i] >> 4] ||
        module[root_separator + 3 + i * 2] != digits[state.active.package.document_sha256[i] & 15]) return false;
    const auto& active = state.active.package;
    u64 sizes[kUpdateGenerationMaximumFiles]{};
    for (u32 i = 0; i < update_generation_file_count(active); ++i) {
        const Utf8View file = update_generation_file_name(active, i); wchar_t name[kUpdatePackageMaximumNameBytes + 1]{};
        for (u32 c = 0; c < file.bytes; ++c) name[c] = static_cast<wchar_t>(file.data[c]);
        signed_update_fixture::child(generation, name, path);
        if (!signed_file(path, &owner.files[i], &sizes[i])) return false;
    }
    const u64 document_bytes = sizes[kUpdateGenerationOfferSlot], package_bytes = sizes[kUpdateGenerationPackageSlot];
    if (!document_bytes || document_bytes > kUpdateManifestMaximumBytes || sizes[kUpdateGenerationSignatureSlot] != 64 ||
        package_bytes != active.package_bytes || package_bytes > 64 * 1024 * 1024) return false;
    char document[kUpdateManifestMaximumBytes]{}; u8 signature[64]{}; u32 scratch[256]{};
    if (!signed_read(owner.files[kUpdateGenerationOfferSlot], document, static_cast<u32>(document_bytes)) ||
        !signed_read(owner.files[kUpdateGenerationSignatureSlot], signature, sizeof(signature)) ||
        !kdf_sha256_initialize(&owner.workspace).ok() || !p256_import_public(&owner.key, P256Kind::ecdsa, {signed_update_fixture::public_point,65}).ok()) return false;
    UpdateStage stage{}; stage.file = owner.files[kUpdateGenerationPackageSlot]; stage.bytes = package_bytes; UpdatePackageDescriptor derived{};
    owner.verification = update_package_verify_installed(&stage, {{document,static_cast<u32>(document_bytes)},{signature,64}}, owner.key, owner.workspace,
        1000, {scratch,256}, {owner.files + kUpdateGenerationFirstMemberSlot, active.member_count}, &derived);
    if (!owner.verification.ok() || !update_package_descriptor_equal(derived, state.active.package)) return false;
    // The parent still holds the install gate. Drop the child's bounded journal
    // reader before acknowledgement so the parent can replace that record.
    if (!maicord::signed_update_fixture::close(&owner.journal)) return false;
    identity->installation = state.installation; identity->transaction = state.transaction; copy_bytes(identity->document_sha256, derived.document_sha256, 32);
    return true;
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_update_handoff_peer_entry() noexcept {
    using namespace maicord;
    const wchar_t* raw = GetCommandLineW(); const ConstSlice<wchar_t> command{raw, length(raw)};
    // Independent ordinary CreateProcess control, before any handoff API or IPC.
    u32 mode_at = 0;
    if (command.count && command.data[mode_at++] == L'"') {
        while (mode_at < command.count && command.data[mode_at] != L'"') ++mode_at;
        if (mode_at < command.count) ++mode_at;
        constexpr wchar_t mode[] = L" --maicord-handoff-native-init-control-v1";
        bool control = mode_at + sizeof(mode) / sizeof(wchar_t) - 1 == command.count;
        for (u32 i = 0; control && i < sizeof(mode) / sizeof(wchar_t) - 1; ++i) control = command.data[mode_at + i] == mode[i];
        if (control) ExitProcess(0);
    }
    UpdateHandoffIdentity expected{}; UpdateHandoffChild child{}; SignedPeerValidation signed_validation{};
    if (!fixture_ids(command, &expected)) ExitProcess(21);
    if (expected.transaction.bytes[0] == 0xe1 && !independently_validate_signed_fixture(signed_validation, &expected)) {
        signed_validation.release(); ExitProcess(34);
    }
    if (!update_handoff_child_open(command, expected, true, &child).ok()) {
        signed_validation.release();
        update_handoff_child_release(&child); ExitProcess(21);
    }
    const u8 scenario = expected.transaction.bytes[0]; u32 code = 0;
    // The synthetic fixture digest embeds a numeric unrelated inheritable handle
    // solely to test exclusion; it is never a production digest or trust token.
    u64 outsider = 0; copy_bytes(&outsider, expected.document_sha256 + 24, sizeof(outsider));
    // The parent signals the same named sentinel once its handoff wait has
    // classified this peer, so the peer stays alive exactly that long.
    HANDLE release = nullptr;
    if (outsider && scenario != 0xe1) {
        // Test-only API resolution from the already mapped OS module. The SDK
        // need not supply Kernelbase.lib; absence is an explicit fixture failure.
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        using CompareHandles = BOOL (WINAPI*)(HANDLE, HANDLE);
        const auto compare = kernelbase ? reinterpret_cast<CompareHandles>(GetProcAddress(kernelbase, "CompareObjectHandles")) : nullptr;
        if (!compare) { update_handoff_child_release(&child); ExitProcess(33); }
        // Capture the object occupying this slot BEFORE opening the sentinel;
        // otherwise OpenEvent itself could reuse the excluded numeric slot.
        HANDLE suspect = nullptr;
        DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(outsider), GetCurrentProcess(), &suspect, 0, FALSE, DUPLICATE_SAME_ACCESS);
        wchar_t name[80]{}; fixture_event_name(expected, name);
        release = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!release || (suspect && compare(suspect, release))) code = 29;
        if (suspect && !CloseHandle(suspect)) code = 31;
    }
    // Live parent: the release event fires first. A parent exit, wait failure
    // or an unusable SYNCHRONIZE parent owner fails the scenario.
    const auto held = [&]() noexcept {
        HANDLE handles[2]{release, child.parent};
        return release && WaitForMultipleObjects(2, handles, FALSE, 10000) == WAIT_OBJECT_0;
    };
    DWORD exit = 0;
    if (GetExitCodeProcess(child.parent, &exit) || GetLastError() != ERROR_ACCESS_DENIED) code = 30;
    if (scenario == 0xe1) {
        AccountHost account{};
        const auto admitted = account_host_load(&account);
        const bool locally_admitted = admitted.ok() && account_host_api_valid(account.api) && !account.instance && !account.login_instance;
        if (!locally_admitted) code = 35;
        // No API start/login/pump/REST function is invoked. The members are
        // verified above, then the linked module's API admission occurs.
        if (locally_admitted && !update_handoff_child_acknowledge(&child, true).ok()) code = 36;
        if (!account_host_release(&account).ok()) code = 37;
        // The test parent stays alive while it awaits this peer's exit, so
        // the parent wait can only reach its deadline; keep that bound short.
        if (locally_admitted) WaitForSingleObject(child.parent, 500);
    } else if (scenario == 1 || scenario == 2 || scenario == 6 || scenario == 7) {
        if (update_handoff_child_acknowledge(&child, false).error != UpdateHandoffError::acknowledgement) code = 22;
        if (scenario == 2) child.identity.transaction.bytes[0] ^= 0x80;
        if (scenario == 6) child.identity.document_sha256[0] ^= 0x80;
        if (scenario == 7) child.identity.installation.bytes[0] ^= 0x80;
        if (!update_handoff_child_acknowledge(&child, true).ok()) code = 23;
        if (!held()) code = 24;
    } else if (scenario == 3) {
        if (!held()) code = 25;
    } else if (scenario == 5) {
        if (!SetEvent(child.event)) code = 26;
        if (!held()) code = 26;
    } else if (scenario != 4) code = 27;
    if (release && !CloseHandle(release)) code = 32;
    if (!update_handoff_child_release(&child).ok()) code = 28;
    if (!signed_validation.release()) code = 38;
    ExitProcess(code);
}

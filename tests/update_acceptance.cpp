#include "maicord/app/update_acceptance.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool write_fixture(const wchar_t* path, const u8* data, u32 count) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, count, &written, nullptr) && written == count;
    return CloseHandle(file) && ok;
}
bool state_equal(const UpdateAcceptanceState& a, const UpdateAcceptanceState& b) noexcept {
    for (u32 i = 0; i < 2; ++i)
        if (a.channels[i].highest_version != b.channels[i].highest_version || a.channels[i].present != b.channels[i].present ||
            !bytes_equal(a.channels[i].manifest_sha256, b.channels[i].manifest_sha256, 32)) return false;
    return true;
}
void append(const wchar_t* base, const wchar_t* suffix, wchar_t* output) noexcept {
    u32 i = 0, j = 0;
    while (base[i]) { output[i] = base[i]; ++i; }
    do { output[i + j] = suffix[j]; } while (suffix[j++]);
}
void checksum(u8* record) noexcept { byte_store_le64(record + 120, hash64_bytes(kHash64Seed, record, 120)); }
} // namespace

void test_update_acceptance(void (*check)(bool, const char*) noexcept) noexcept {
    UpdateAcceptanceState state{};
    state.channels[0].highest_version = 6;
    state.channels[0].present = true;
    for (u32 i = 0; i < 32; ++i) state.channels[0].manifest_sha256[i] = static_cast<u8>(i);
    state.channels[1].highest_version = ~u64{0};
    state.channels[1].present = true;
    for (auto& byte : state.channels[1].manifest_sha256) byte = 0xC5;
    u8 record[kUpdateAcceptanceRecordBytes + 1]{};
    UpdateAcceptanceState decoded{};
    check(update_acceptance_encode(state, {record, kUpdateAcceptanceRecordBytes}).ok() &&
        record[8] == 1 && record[12] == 128 && record[16] == 6 && record[56] == 1 &&
        update_acceptance_decode({record, kUpdateAcceptanceRecordBytes}, &decoded).ok() && state_equal(state, decoded),
        "acceptance: independent channels and full-width versions survive fixed portable record encoding");
    const auto sentinel = state;
    auto reset = [&]() noexcept { decoded = sentinel; return update_acceptance_encode(state, {record, 128}).ok(); };
    auto rejected = [&](UpdateAcceptanceError expected, u32 count = 128) noexcept {
        return update_acceptance_decode({record, count}, &decoded).error == expected && state_equal(decoded, sentinel);
    };
    reset(); record[24] ^= 1;
    check(rejected(UpdateAcceptanceError::checksum), "acceptance: changed document digest is detected without output publication");
    reset(); record[120] ^= 1;
    check(rejected(UpdateAcceptanceError::checksum), "acceptance: corrupt checksum fails closed");
    reset(); record[8] = 2; checksum(record);
    check(rejected(UpdateAcceptanceError::format), "acceptance: unknown record versions cannot erase or reinterpret policy");
    reset(); record[57] = 1; checksum(record);
    check(rejected(UpdateAcceptanceError::format), "acceptance: reserved channel fields fail even with a matching checksum");
    reset(); record[56] = 0; checksum(record);
    check(rejected(UpdateAcceptanceError::format), "acceptance: absent channel cannot hide a nonzero rollback floor or digest");
    reset(); byte_store_le64(record + 16, 0); checksum(record);
    check(rejected(UpdateAcceptanceError::format), "acceptance: present channel requires a nonzero accepted version");
    reset();
    check(rejected(UpdateAcceptanceError::size, 127) && rejected(UpdateAcceptanceError::size, 129),
        "acceptance: truncated and trailing records both fail exact schema");
    const auto invalid_state = UpdateAcceptanceState{{{1, {}, false}, {}}};
    u8 encoded_snapshot[128]; copy_bytes(encoded_snapshot, record, 128);
    check(update_acceptance_encode(invalid_state, {record, 128}).error == UpdateAcceptanceError::invalid_input &&
        bytes_equal(record, encoded_snapshot, 128), "acceptance: invalid state cannot partially alter an encoded record");

    // Fixtures are isolated in Windows TEMP, never the project or actual enrollment.
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{}, lock_path[MAX_PATH + 8]{}, new_path[MAX_PATH + 8]{};
    const DWORD directory_chars = GetTempPathW(MAX_PATH, directory);
    const bool created = directory_chars && directory_chars < MAX_PATH && GetTempFileNameW(directory, L"MUA", 0, path);
    check(created, "acceptance: isolated external temporary storage fixture is created");
    if (!created) return;
    append(path, L".lock", lock_path); append(path, L".new", new_path);
    UpdateAcceptanceTransaction owner{}, peer{};
    struct Cleanup {
        UpdateAcceptanceTransaction& a; UpdateAcceptanceTransaction& b;
        const wchar_t* path; const wchar_t* lock; const wchar_t* temporary;
        ~Cleanup() noexcept {
            update_acceptance_release(&a); update_acceptance_release(&b);
            DeleteFileW(temporary); DeleteFileW(path); DeleteFileW(lock);
        }
    } cleanup{owner, peer, path, lock_path, new_path};
    u32 count = 0; while (path[count]) ++count;
    const ConstSlice<wchar_t> storage{path, count};
    check(DeleteFileW(path), "acceptance: first-use fixture starts without an acceptance record");
    decoded = sentinel;
    check(update_acceptance_load(storage, &decoded).error == UpdateAcceptanceError::missing && state_equal(decoded, sentinel) &&
        update_acceptance_begin(storage, &owner).error == UpdateAcceptanceError::missing && !owner.lock,
        "acceptance: missing enrolled storage is explicit and never silently bootstraps or changes outputs");
    const auto begun = update_acceptance_begin(storage, &owner, true);
    check(begun.ok() && owner.lock && !owner.record_present,
        "acceptance: caller-authorized first-use enrollment captures empty storage under an owned lock");
    if (!begun.ok()) return;
    const auto blocked = update_acceptance_begin(storage, &peer, true);
    check(blocked.error == UpdateAcceptanceError::lock && blocked.native_error == ERROR_SHARING_VIOLATION && !peer.lock &&
        !DeleteFileW(lock_path) && GetLastError() == ERROR_SHARING_VIOLATION,
        "acceptance: competing handle and lock deletion are denied through the entire transaction");

    KdfSha256Workspace workspace{};
    P256Key signer{}, trusted{};
    struct ReleaseCrypto {
        KdfSha256Workspace& workspace; P256Key& signer; P256Key& trusted;
        ~ReleaseCrypto() noexcept { p256_release(&trusted); p256_release(&signer); kdf_sha256_release(&workspace); }
    } release_crypto{workspace, signer, trusted};
    u8 scalar[32]{}; scalar[31] = 1; // test-only private key, never shipped trust inventory
    u8 point[65]{}, signature[64]{};
    const bool initialized = kdf_sha256_initialize(&workspace).ok() &&
        p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&trusted, P256Kind::ecdsa, {point, 65}).ok();
    check(initialized, "acceptance: test-only detached signer and independent trusted public key initialize");
    if (!initialized) return;
    u32 scratch[256]{};
    UpdateManifestPolicy policy{4, 0, 100, 65536, 1000, UpdateChannel::stable};
    UpdateManifest approved{};
    approved.version = 99;
    const auto six = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-6.zip"})");
    const auto seven = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":7,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-7.zip"})");
    const auto five = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":5,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-5.zip"})");
    const auto preview = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"preview","version":9,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-9.zip"})");
    auto sign = [&](Utf8View document) noexcept {
        const ConstSlice<u8> parts[]{
            {reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(document.data), document.bytes}};
        u8 digest[32]{};
        return sha256(workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(signer, {digest, 32}, {signature, 64}).ok();
    };
    auto admit = [&](Utf8View document) noexcept {
        return update_acceptance_verify_commit(&owner, document, {signature, 64}, trusted, workspace,
            &policy, {scratch, 256}, &approved);
    };
    check(sign(six), "acceptance: first offer is signed with exact update-domain bytes");
    signature[7] ^= 1;
    const auto untouched_policy = policy;
    const auto untouched_approved = approved;
    const auto untouched_owner = owner;
    const auto bad_signature = admit(six);
    check(bad_signature.error == UpdateAcceptanceError::manifest && bad_signature.manifest_error == UpdateManifestError::signature &&
        bytes_equal(&policy, &untouched_policy, sizeof(policy)) && bytes_equal(&approved, &untouched_approved, sizeof(approved)) &&
        bytes_equal(&owner, &untouched_owner, sizeof(owner)),
        "acceptance: invalid publisher signature changes no state, global policy or approved manifest");
    check(sign(six) && admit(six).ok() && approved.version == 6 && policy.highest_accepted_version == 6 &&
        policy.has_accepted_manifest_digest && bytes_equal(policy.accepted_manifest_sha256, approved.manifest_sha256, 32) &&
        update_acceptance_load(storage, &decoded).ok() && decoded.channels[0].highest_version == 6 &&
        bytes_equal(decoded.channels[0].manifest_sha256, approved.manifest_sha256, 32) && !decoded.channels[1].present,
        "acceptance: successful durable commit publishes exact signed document identity before artifact admission");
    const auto accepted_state = owner.state;
    const auto accepted_policy = policy;
    const auto accepted_manifest = approved;
    HANDLE deny_read = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(deny_read != INVALID_HANDLE_VALUE, "acceptance: noncooperating writer fixture prevents a stable record read");
    if (deny_read != INVALID_HANDLE_VALUE) {
        decoded = sentinel;
        const auto unreadable = update_acceptance_load(storage, &decoded);
        const auto no_admission = sign(seven) ? admit(seven) : UpdateAcceptanceResult{UpdateAcceptanceError::invalid_input};
        check(unreadable.error == UpdateAcceptanceError::open && unreadable.native_error == ERROR_SHARING_VIOLATION &&
            no_admission.error == UpdateAcceptanceError::open && state_equal(decoded, sentinel) &&
            bytes_equal(&policy, &accepted_policy, sizeof(policy)) && bytes_equal(&approved, &accepted_manifest, sizeof(approved)),
            "acceptance: read IO failure is distinct from missing and publishes no new artifact authority");
        CloseHandle(deny_read);
    }
    check(write_fixture(new_path, record, 1), "acceptance: stale sibling fixture exists without being silently overwritten");
    check(sign(six) && admit(six).ok() && state_equal(owner.state, accepted_state),
        "acceptance: identical signed-document retry uses existing durable acceptance without another write");
    const auto stale_sibling = sign(seven) ? admit(seven) : UpdateAcceptanceResult{UpdateAcceptanceError::invalid_input};
    check(stale_sibling.error == UpdateAcceptanceError::open &&
        bytes_equal(&policy, &accepted_policy, sizeof(policy)) && bytes_equal(&approved, &accepted_manifest, sizeof(approved)) &&
        state_equal(owner.state, accepted_state) && update_acceptance_load(storage, &decoded).ok() && state_equal(decoded, accepted_state),
        "acceptance: failed sibling creation preserves prior disk state, transaction, policy and artifact authority");
    check(DeleteFileW(new_path), "acceptance: test-owned stale sibling is removed explicitly");
    HANDLE deny_replace = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(deny_replace != INVALID_HANDLE_VALUE, "acceptance: destination fixture denies rename while allowing complete reads");
    if (deny_replace != INVALID_HANDLE_VALUE) {
        const auto failed = sign(seven) ? admit(seven) : UpdateAcceptanceResult{UpdateAcceptanceError::invalid_input};
        // MoveFileEx may report ACCESS_DENIED for a destination lacking delete
        // sharing; its contract does not guarantee CreateFile's sharing code.
        check(failed.error == UpdateAcceptanceError::commit &&
            (failed.native_error == ERROR_SHARING_VIOLATION || failed.native_error == ERROR_ACCESS_DENIED) &&
            bytes_equal(&policy, &accepted_policy, sizeof(policy)) && bytes_equal(&approved, &accepted_manifest, sizeof(approved)) &&
            state_equal(owner.state, accepted_state), "acceptance: failed atomic replacement publishes no new policy or approved manifest");
        CloseHandle(deny_replace);
        check(update_acceptance_load(storage, &decoded).ok() && state_equal(decoded, accepted_state) &&
            GetFileAttributesW(new_path) == INVALID_FILE_ATTRIBUTES,
            "acceptance: failed replacement retains complete old record and cleans only its own temporary sibling");
    }
    char changed[kUpdateManifestMaximumBytes]{};
    copy_bytes(changed, six.data, six.bytes); changed[six.bytes] = ' ';
    const auto changed_result = sign({changed, six.bytes + 1}) ? admit({changed, six.bytes + 1}) :
        UpdateAcceptanceResult{UpdateAcceptanceError::invalid_input};
    const auto lower_result = sign(five) ? admit(five) : UpdateAcceptanceResult{UpdateAcceptanceError::invalid_input};
    check(changed_result.error == UpdateAcceptanceError::manifest && changed_result.manifest_error == UpdateManifestError::rollback &&
        lower_result.error == UpdateAcceptanceError::manifest && lower_result.manifest_error == UpdateManifestError::rollback &&
        bytes_equal(&policy, &accepted_policy, sizeof(policy)) && bytes_equal(&approved, &accepted_manifest, sizeof(approved)),
        "acceptance: changed same-version signed bytes and lower-version replay preserve durable authority");

    // Simulate process restart using a fresh owner and fresh policy configuration.
    check(update_acceptance_release(&owner).ok() && update_acceptance_begin(storage, &peer).ok(),
        "acceptance: explicit release permits a fresh process-owner snapshot to reload durable acceptance");
    UpdateManifestPolicy fresh{4, 0, 100, 65536, 1000, UpdateChannel::stable}, captured{};
    check(update_acceptance_capture_policy(peer, fresh, &captured).ok() && captured.highest_accepted_version == 6 &&
        captured.has_accepted_manifest_digest && bytes_equal(captured.accepted_manifest_sha256, accepted_manifest.manifest_sha256, 32) &&
        fresh.highest_accepted_version == 0,
        "acceptance: restart capture restores rollback floor and exact digest without changing caller policy");
    check(update_acceptance_release(&peer).ok() && update_acceptance_begin(storage, &owner).ok(),
        "acceptance: fresh transaction reacquires the shared serialization guard");
    policy = fresh; policy.channel = UpdateChannel::preview;
    check(sign(preview) && admit(preview).ok() && policy.highest_accepted_version == 9 &&
        update_acceptance_load(storage, &decoded).ok() && decoded.channels[0].highest_version == 6 && decoded.channels[1].highest_version == 9,
        "acceptance: preview admission preserves stable channel history independently");
    policy = fresh;
    check(sign(seven) && admit(seven).ok() && policy.highest_accepted_version == 7 &&
        update_acceptance_load(storage, &decoded).ok() && decoded.channels[0].highest_version == 7 && decoded.channels[1].highest_version == 9,
        "acceptance: later stable advancement preserves preview acceptance in the same atomic record");

    const auto latest_state = owner.state;
    const auto latest_policy = policy;
    const auto latest_approved = approved;
    check(update_acceptance_encode(latest_state, {record, 128}).ok(), "acceptance: exact captured disk snapshot is available for corruption fixtures");
    record[24] ^= 1;
    check(write_fixture(path, record, 128) && sign(seven) && admit(seven).error == UpdateAcceptanceError::checksum &&
        bytes_equal(&policy, &latest_policy, sizeof(policy)) && bytes_equal(&approved, &latest_approved, sizeof(approved)),
        "acceptance: disk corruption introduced after capture prevents retry admission without policy mutation");
    check(update_acceptance_encode(latest_state, {record, 128}).ok() && write_fixture(path, record, 128),
        "acceptance: test restores its own valid disk fixture");
    auto different = latest_state; different.channels[0].highest_version = 5;
    check(update_acceptance_encode(different, {record, 128}).ok() && write_fixture(path, record, 128) &&
        update_acceptance_capture_policy(owner, fresh, &captured).error == UpdateAcceptanceError::state,
        "acceptance: changed valid disk snapshot cannot be accepted against stale transaction state");
    check(update_acceptance_encode(latest_state, {record, 128}).ok() && write_fixture(path, record, 128) &&
        update_acceptance_release(&owner).ok(), "acceptance: valid record restored before fresh-owner corruption checks");
    decoded = sentinel;
    check(write_fixture(path, record, 127) && update_acceptance_load(storage, &decoded).error == UpdateAcceptanceError::size &&
        update_acceptance_begin(storage, &peer, true).error == UpdateAcceptanceError::size && !peer.lock && state_equal(decoded, sentinel),
        "acceptance: truncated storage fails closed even when caller authorizes first-use provisioning");
    check(write_fixture(path, record, 129) && update_acceptance_load(storage, &decoded).error == UpdateAcceptanceError::size,
        "acceptance: oversized storage is rejected before a bounded read");
    record[120] ^= 1;
    check(write_fixture(path, record, 128) && update_acceptance_begin(storage, &peer, true).error == UpdateAcceptanceError::checksum && !peer.lock,
        "acceptance: checksum corruption cannot trigger first-use recovery or lock ownership publication");
    check(update_acceptance_encode(latest_state, {record, 128}).ok() && write_fixture(path, record, 128) &&
        update_acceptance_begin(storage, &owner).ok(), "acceptance: restored record is reopened for rollback-floor and erase checks");
    fresh.highest_accepted_version = 8;
    check(update_acceptance_capture_policy(owner, fresh, &captured).error == UpdateAcceptanceError::state,
        "acceptance: stronger caller rollback floor is never lowered to persisted storage");
    check(DeleteFileW(path) && sign(seven) && admit(seven).error == UpdateAcceptanceError::missing &&
        bytes_equal(&policy, &latest_policy, sizeof(policy)) && bytes_equal(&approved, &latest_approved, sizeof(approved)),
        "acceptance: erased enrolled record prevents admission and preserves global authority");
    check(update_acceptance_release(&owner).ok() && update_acceptance_begin(storage, &peer).error == UpdateAcceptanceError::missing && !peer.lock,
        "acceptance: restart after enrolled record erasure fails closed by default");
    check(CreateDirectoryW(path, nullptr) != 0 && !update_acceptance_begin(storage, &peer, true).ok() && !peer.lock &&
        RemoveDirectoryW(path) != 0, "acceptance: existing directory cannot be treated as missing first-use record storage");

    constexpr wchar_t relative[] = L"acceptance.bin", unc[] = L"\\\\server\\share\\acceptance.bin";
    constexpr wchar_t traversal[] = L"D:\\MaiCord\\..\\acceptance.bin", ads[] = L"D:\\acceptance.bin:stream";
    constexpr wchar_t device[] = L"D:\\NUL.bin", trailing[] = L"D:\\acceptance.bin.";
    const ConstSlice<wchar_t> invalid_paths[]{
        {relative, sizeof(relative) / sizeof(wchar_t) - 1}, {unc, sizeof(unc) / sizeof(wchar_t) - 1},
        {traversal, sizeof(traversal) / sizeof(wchar_t) - 1}, {ads, sizeof(ads) / sizeof(wchar_t) - 1},
        {device, sizeof(device) / sizeof(wchar_t) - 1}, {trailing, sizeof(trailing) / sizeof(wchar_t) - 1}};
    for (auto invalid : invalid_paths)
        check(!update_acceptance_begin(invalid, &peer, true).ok() && !peer.lock,
            "acceptance: relative, network, traversal, ADS, DOS-device and trailing-dot storage paths are refused");
    check(update_acceptance_begin({reinterpret_cast<const wchar_t*>(~usize{0} - 1), 4}, &peer, true).error == UpdateAcceptanceError::invalid_input &&
        update_acceptance_decode({nullptr, 128}, &decoded).error == UpdateAcceptanceError::invalid_input &&
        update_acceptance_release(nullptr).error == UpdateAcceptanceError::invalid_input,
        "acceptance: wrapping and null arguments are refused before dereference or output mutation");
}
} // namespace maicord

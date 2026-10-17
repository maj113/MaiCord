#include "maicord/app/update_generation.hpp"
#include "maicord/app/account_module_version.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
UpdatePairDescriptor fixture_pair(u64 version, u8 marker) noexcept {
    UpdatePairDescriptor p{}; p.version = version; p.package_bytes = 1000;
    p.account_ordinal = kAccountModuleApiOrdinal; p.members[0].bytes = 120; p.members[1].bytes = 80;
    for (u32 i = 0; i < 32; ++i) {
        p.document_sha256[i] = marker; p.package_sha256[i] = static_cast<u8>(marker + 1);
        p.members[0].sha256[i] = static_cast<u8>(marker + 2); p.members[1].sha256[i] = static_cast<u8>(marker + 3);
    }
    return p;
}
UpdateGenerationState fixture_state() noexcept {
    UpdateGenerationState s{}; s.installation.bytes[0] = 1; s.sequence = 1;
    s.active = {fixture_pair(5, 0x11), true}; return s;
}
bool state_equal(const UpdateGenerationState& a, const UpdateGenerationState& b) noexcept {
    for (u32 i = 0; i < 5; ++i) if (a.disposal.files[i].volume != b.disposal.files[i].volume || a.disposal.files[i].file != b.disposal.files[i].file) return false;
    return bytes_equal(a.installation.bytes, b.installation.bytes, 16) && bytes_equal(a.transaction.bytes, b.transaction.bytes, 16) &&
        a.sequence == b.sequence && a.phase == b.phase && a.active.present == b.active.present &&
        a.previous.present == b.previous.present && a.candidate.present == b.candidate.present &&
        update_generation_pair_equal(a.active.pair, b.active.pair) && update_generation_pair_equal(a.previous.pair, b.previous.pair) &&
        update_generation_pair_equal(a.candidate.pair, b.candidate.pair) && a.disposal.target == b.disposal.target &&
        a.disposal.directory.volume == b.disposal.directory.volume && a.disposal.directory.file == b.disposal.directory.file;
}
bool roundtrip(const UpdateGenerationState& s) noexcept {
    u8 record[kUpdateGenerationRecordBytes]{}; UpdateGenerationState reopened{};
    return update_generation_encode(s, {record, sizeof(record)}).ok() &&
        update_generation_decode({record, sizeof(record)}, &reopened).ok() && state_equal(s, reopened);
}
void checksum(u8* b) noexcept { byte_store_le64(b + kUpdateGenerationRecordBytes - 8, hash64_bytes(kHash64Seed, b, kUpdateGenerationRecordBytes - 8)); }
void append(const wchar_t* base, const wchar_t* suffix, wchar_t* output) noexcept {
    u32 n = 0; while (base[n]) { output[n] = base[n]; ++n; }
    for (u32 i = 0;; ++i) { output[n + i] = suffix[i]; if (!suffix[i]) break; }
}
u32 length(const wchar_t* p) noexcept { u32 n = 0; while (p[n]) ++n; return n; }
bool ancestry(const wchar_t* root, UpdateGenerationDirectoryIdentity* ids, u32* count) noexcept {
    wchar_t path[MAX_PATH]{}; append(root, L"", path); *count = 0;
    const u32 n = length(path);
    for (u32 i = 2; i <= n; ++i) if (i == 2 || i == n || path[i] == L'\\') {
        const u32 end = i == 2 ? 3 : i; const wchar_t saved = path[end]; path[end] = 0;
        HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); path[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
        const bool closed = CloseHandle(h) != FALSE; if (!ok || !closed || *count == kUpdateGenerationMaximumAncestors) return false;
        ids[*count] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow}; ++*count;
    }
    return true;
}
struct FixtureAuthority { bool accept; u8 refused_digest; u32 calls; };
// Synthetic gate/codec fixture only. Production uses the package authenticity
// adapter; this callback intentionally has no assertion of publisher authority.
bool fixture_authority(void* opaque, UpdateGenerationTransaction*, const UpdatePairDescriptor& p, DWORD* native) noexcept {
    auto& a = *static_cast<FixtureAuthority*>(opaque); ++a.calls;
    const bool ok = a.accept && p.document_sha256[0] != a.refused_digest;
    if (native) *native = ok ? 0 : ERROR_INVALID_DATA; return ok;
}
bool write_new(const wchar_t* path, const u8* bytes, u32 count) noexcept {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const bool ok = WriteFile(h, bytes, count, &written, nullptr) && written == count && FlushFileBuffers(h);
    return CloseHandle(h) && ok;
}
bool dispose_file(HANDLE* h) noexcept {
    if (!*h) return true; FILE_DISPOSITION_INFO info{TRUE};
    if (!SetFileInformationByHandle(*h, FileDispositionInfo, &info, sizeof(info)) || !CloseHandle(*h)) return false;
    *h = nullptr; return true;
}
bool dispose_candidate_fixture(UpdateGenerationTransaction& owner, UpdateGenerationDirectory& directory,
    const UpdateGenerationAuthenticator& auth) noexcept {
    const auto& pair = owner.state.candidate.pair;
    if (!update_generation_directory_open(&owner, {pair.document_sha256, 32}, true, &directory).ok()) return false;
    UpdateGenerationDisposalIntent intent{}; intent.target = UpdateGenerationDisposalTarget::candidate; intent.directory = directory.identity;
    for (u32 i = 0; i < 5; ++i) {
        HANDLE file = nullptr;
        if (!update_generation_file_open(&directory, static_cast<UpdateGenerationFile>(i), true, &file).ok()) return false;
        BY_HANDLE_FILE_INFORMATION info{}; const u8 payload = static_cast<u8>(i + 1); DWORD written = 0;
        const bool captured = WriteFile(file, &payload, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file) && GetFileInformationByHandle(file, &info);
        if (!CloseHandle(file) || !captured) return false;
        intent.files[i] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    }
    UpdateGenerationState proposed{};
    if (!update_generation_disposal_begin(owner.state, intent, &proposed).ok() || !update_generation_commit(&owner, proposed, auth).ok() ||
        !update_generation_directory_authorize_disposal(&directory, auth).ok() || !update_generation_disposal_inventory(&directory).ok()) return false;
    for (u32 i = 0; i < 5; ++i) {
        wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
        if (!update_generation_path(owner, {pair.document_sha256, 32}, static_cast<UpdateGenerationFile>(i),
            {path, sizeof(path) / sizeof(path[0])}, &count).ok()) return false;
        HANDLE file = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE || !dispose_file(&file)) return false;
    }
    if (!update_generation_directory_release(&directory, true).ok()) return false;
    directory = {};
    return update_generation_finish(owner.state, true, &proposed).ok() && update_generation_commit(&owner, proposed, auth).ok();
}
} // namespace

void test_update_generation(void (*check)(bool, const char*) noexcept) noexcept {
    const auto initial = fixture_state(); const auto new_pair = fixture_pair(6, 0x33);
    UpdateGenerationIdentity transaction{}; transaction.bytes[0] = 7;
    UpdateGenerationState prepared{}, pending{}, acknowledged{}, stable{}, rollback{}, finished{};
    check(roundtrip(initial) && update_generation_prepare(initial, transaction, new_pair, &prepared).ok() && roundtrip(prepared) &&
        update_generation_activate(prepared, &pending).ok() && roundtrip(pending),
        "generation: complete whole-pair references survive restart at stable/prepared/pending boundaries");
    UpdateGenerationRecovery plan{};
    check(update_generation_recovery(initial, &plan).ok() && plan == UpdateGenerationRecovery::launch_active &&
        update_generation_recovery(prepared, &plan).ok() && plan == UpdateGenerationRecovery::retain_prepared &&
        update_generation_recovery(pending, &plan).ok() && plan == UpdateGenerationRecovery::roll_back,
        "generation: prepared bytes never select a new pair and lost acknowledgement always plans rollback");
    auto sentinel = initial; auto wrong_transaction = transaction; wrong_transaction.bytes[1] = 1;
    u8 wrong_digest[32]{}; copy_bytes(wrong_digest, new_pair.document_sha256, 32); wrong_digest[31] ^= 1;
    check(update_generation_acknowledge(pending, wrong_transaction, {new_pair.document_sha256, 32}, &sentinel).error == UpdateGenerationError::identity &&
        state_equal(sentinel, initial) && update_generation_acknowledge(pending, transaction, {wrong_digest, 32}, &sentinel).error == UpdateGenerationError::identity &&
        state_equal(sentinel, initial), "generation: wrong transaction and generation acknowledgement fail without state publication");
    check(update_generation_acknowledge(pending, transaction, {new_pair.document_sha256, 32}, &acknowledged).ok() && roundtrip(acknowledged) &&
        update_generation_recovery(acknowledged, &plan).ok() && plan == UpdateGenerationRecovery::finish_acknowledgement &&
        update_generation_finish(acknowledged, false, &stable).ok() && roundtrip(stable) && stable.previous.present &&
        update_generation_pair_equal(stable.active.pair, new_pair),
        "generation: durable local acknowledgement preserves the sole previous pair across finishing restart");
    sentinel = initial;
    check(!update_generation_acknowledge(stable, transaction, {new_pair.document_sha256, 32}, &sentinel).ok() && state_equal(sentinel, initial) &&
        !update_generation_prepare(stable, transaction, fixture_pair(7, 0x55), &sentinel).ok(),
        "generation: stale acknowledgement and a third retained generation are refused");
    check(update_generation_rollback(pending, &rollback).ok() && roundtrip(rollback) && rollback.candidate.present &&
        update_generation_pair_equal(rollback.active.pair, initial.active.pair) &&
        !update_generation_finish(rollback, false, &sentinel).ok() && !update_generation_finish(rollback, true, &finished).ok(),
        "generation: rollback cannot finalize disposal from a caller completion boolean without durable intent");
    UpdateGenerationDisposalIntent candidate_intent{}; candidate_intent.target = UpdateGenerationDisposalTarget::candidate;
    candidate_intent.directory = {7, 10}; for (u32 i = 0; i < 5; ++i) candidate_intent.files[i] = {7, 20 + i};
    UpdateGenerationState disposing{};
    check(update_generation_disposal_begin(rollback, candidate_intent, &disposing).ok() && roundtrip(disposing) &&
        update_generation_finish(disposing, true, &finished).ok() && update_generation_prepare(finished, transaction, new_pair, &sentinel).ok(),
        "generation: captured disposal intent survives codec restart and finalization permits identical admitted retry");
    UpdateAcceptanceState high_water{}; high_water.channels[0] = {6, {}, true};
    copy_bytes(high_water.channels[0].manifest_sha256, new_pair.document_sha256, 32);
    u8 acceptance_before[kUpdateAcceptanceRecordBytes]{}, acceptance_after[kUpdateAcceptanceRecordBytes]{};
    update_acceptance_encode(high_water, {acceptance_before, sizeof(acceptance_before)});
    update_generation_rollback(pending, &rollback);
    update_acceptance_encode(high_water, {acceptance_after, sizeof(acceptance_after)});
    check(bytes_equal(acceptance_before, acceptance_after, sizeof(acceptance_before)), "generation: recovery does not lower network accepted high-water state");
    UpdateGenerationState no_previous{};
    auto previous_intent = candidate_intent; previous_intent.target = UpdateGenerationDisposalTarget::previous;
    UpdateGenerationState pruning{};
    check(!update_generation_forget_previous(stable, false, &sentinel).ok() &&
        !update_generation_forget_previous(stable, true, &sentinel).ok() &&
        update_generation_disposal_begin(stable, previous_intent, &pruning).ok() && roundtrip(pruning) &&
        update_generation_recovery(pruning, &plan).ok() && plan == UpdateGenerationRecovery::discard_previous &&
        update_generation_forget_previous(pruning, true, &no_previous).ok() && !no_previous.previous.present &&
        update_generation_prepare(no_previous, transaction, fixture_pair(7, 0x55), &sentinel).ok(),
        "generation: previous disposal must complete before admitting the next bounded candidate");
    auto duplicate_intent = candidate_intent; duplicate_intent.files[1] = duplicate_intent.files[0];
    check(!update_generation_disposal_begin(rollback, duplicate_intent, &sentinel).ok() &&
        !update_generation_disposal_begin(stable, candidate_intent, &sentinel).ok(),
        "generation: duplicate or wrong-target intent identities cannot authorize a disposal plan");
    auto no_finish_sequence = rollback; no_finish_sequence.sequence = ~u64{0} - 1; sentinel = initial;
    check(!update_generation_disposal_begin(no_finish_sequence, candidate_intent, &sentinel).ok() && state_equal(sentinel, initial),
        "generation: disposal admission reserves its final journal sequence before physical deletion");

    u8 record[kUpdateGenerationRecordBytes + 1]{};
    auto reset = [&]() noexcept { sentinel = initial; return update_generation_encode(pending, {record, kUpdateGenerationRecordBytes}).ok(); };
    auto rejected = [&](UpdateGenerationError expected, u32 n = kUpdateGenerationRecordBytes) noexcept {
        return update_generation_decode({record, n}, &sentinel).error == expected && state_equal(sentinel, initial);
    };
    reset(); record[80] ^= 1;
    check(rejected(UpdateGenerationError::checksum), "generation: member/provenance mutation without journal checksum fails closed");
    reset(); record[8] = 3; checksum(record);
    check(rejected(UpdateGenerationError::format), "generation: unknown seed journal version is refused even with matching checksum");
    reset(); record[57] = 1; checksum(record);
    check(rejected(UpdateGenerationError::format), "generation: reserved journal bytes cannot hide state");
    reset(); record[56] = static_cast<u8>(UpdateGenerationPhase::stable); checksum(record);
    check(rejected(UpdateGenerationError::format), "generation: ambiguous active transaction cannot masquerade as acknowledged stable state");
    reset();
    check(rejected(UpdateGenerationError::size, kUpdateGenerationRecordBytes - 1) && rejected(UpdateGenerationError::size, kUpdateGenerationRecordBytes + 1),
        "generation: torn and appended journal records preserve previous output");
    auto exhausted = initial; exhausted.sequence = ~u64{0}; sentinel = initial;
    check(!update_generation_prepare(exhausted, transaction, new_pair, &sentinel).ok() && state_equal(sentinel, initial),
        "generation: sequence wrap cannot alias an earlier install transaction");
    update_generation_encode(disposing, {record, kUpdateGenerationRecordBytes}); sentinel = initial;
    record[608] ^= 1;
    check(rejected(UpdateGenerationError::checksum), "generation: torn disposal identity is refused before any output publication");
    update_generation_encode(disposing, {record, kUpdateGenerationRecordBytes}); record[593] = 1; checksum(record);
    check(rejected(UpdateGenerationError::format), "generation: reserved disposal intent metadata remains strictly zero");
    update_generation_encode(initial, {record, kUpdateGenerationRecordBytes});
    u8 legacy[640]{}; copy_bytes(legacy, record, 592); legacy[7] = '1'; legacy[8] = 1;
    byte_store_le32(legacy + 12, sizeof(legacy));
    byte_store_le64(legacy + 632, hash64_bytes(kHash64Seed, legacy, 632));
    check(update_generation_decode({legacy, sizeof(legacy)}, &sentinel).error == UpdateGenerationError::size && state_equal(sentinel, initial),
        "generation: unreleased v1 journal fails closed without implicit format migration or first-use reset");

    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, state_path[MAX_PATH + 32]{}, lock_path[MAX_PATH + 32]{}, temporary_path[MAX_PATH + 32]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    bool created = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"MUG", 0, root);
    if (created) created = DeleteFileW(root) && CreateDirectoryW(root, nullptr);
    check(created, "generation: fixtures create one isolated owned root outside the project"); if (!created) return;
    append(root, L"\\install.state", state_path); append(root, L"\\install.lock", lock_path); append(root, L"\\install.state.new", temporary_path);
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 id_count = 0;
    const bool enrolled = ancestry(root, ids, &id_count);
    check(enrolled, "generation: fixture records explicit identity of every existing directory ancestor");
    UpdateGenerationEnrollment enrollment{}; enrollment.root = {root, length(root)}; enrollment.installation = initial.installation;
    enrollment.ancestors = {ids, id_count}; enrollment.publisher_inventory_sha256[0] = 0xAA; enrollment.explicitly_enrolled = true;
    UpdateGenerationTransaction owner{}, peer{}; UpdateGenerationDirectory directory{};
    HANDLE writable = nullptr, sealed = nullptr;
    struct Cleanup {
        UpdateGenerationTransaction& owner; UpdateGenerationTransaction& peer; UpdateGenerationDirectory& directory;
        HANDLE& writable; HANDLE& sealed; const wchar_t* state; const wchar_t* lock; const wchar_t* temporary; const wchar_t* root;
        ~Cleanup() noexcept {
            dispose_file(&sealed); dispose_file(&writable);
            if (directory.created) update_generation_directory_release(&directory, true);
            else update_generation_directory_release(&directory, false);
            update_generation_release(&peer); update_generation_release(&owner);
            // These exact paths were created only by this fixture; no enumeration.
            DeleteFileW(temporary); DeleteFileW(state); DeleteFileW(lock); RemoveDirectoryW(root);
        }
    } cleanup{owner, peer, directory, writable, sealed, state_path, lock_path, temporary_path, root};
    if (!enrolled) return;
    const auto ordinary_begin = update_generation_begin(enrollment, &owner);
    FixtureAuthority first_use_authority{true, 0, 0};
    const UpdateGenerationAuthenticator first_use_auth{&first_use_authority, fixture_authority};
    check(ordinary_begin.error == UpdateGenerationError::open && owner.gate && !owner.provisioning_admitted &&
        update_generation_commit(&owner, initial, first_use_auth).error == UpdateGenerationError::state &&
        !owner.record_present && GetFileAttributesW(state_path) == INVALID_FILE_ATTRIBUTES &&
        update_generation_release(&owner).ok(),
        "generation: ignoring failed ordinary begin cannot silently provision a missing journal");
    check(write_new(state_path, legacy, sizeof(legacy)) && update_generation_begin(enrollment, &owner, true).error == UpdateGenerationError::size &&
        !owner.provisioning_admitted && !owner.record_present && GetFileAttributesW(state_path) != INVALID_FILE_ATTRIBUTES && update_generation_release(&owner).ok(),
        "generation: existing valid v1 journal refuses explicit provisioning without migration and remains intact");
    check(DeleteFileW(state_path) != FALSE, "generation: fixture explicitly removes only its own legacy journal after verifying no automatic migration");
    auto denied_enrollment = enrollment; denied_enrollment.explicitly_enrolled = false;
    check(update_generation_begin(denied_enrollment, &owner, true).error == UpdateGenerationError::enrollment && !owner.gate,
        "generation: a caller path cannot substitute for explicit installation enrollment");
    auto wrong_enrollment = enrollment; UpdateGenerationDirectoryIdentity wrong_ids[kUpdateGenerationMaximumAncestors]{};
    for (u32 i = 0; i < id_count; ++i) wrong_ids[i] = ids[i]; wrong_ids[id_count - 1].file ^= 1;
    wrong_enrollment.ancestors = {wrong_ids, id_count};
    check(update_generation_begin(wrong_enrollment, &owner, true).error == UpdateGenerationError::identity &&
        update_generation_release(&owner).ok(), "generation: a substituted directory identity cannot acquire the install gate");
    constexpr const wchar_t* bad_paths[]{L"C:\\temp\\..\\owned", L"C:\\temp\\owned:stream", L"\\\\server\\share\\owned", L"C:\\temp\\owned.", L"C:\\temp\\CON"};
    bool all_bad = true;
    for (auto path : bad_paths) { auto e = enrollment; e.root = {path, length(path)}; all_bad &= update_generation_begin(e, &owner, true).error == UpdateGenerationError::path; }
    check(all_bad, "generation: dot traversal, alternate stream, UNC, trailing dot and device aliases are refused before opening");
    check(update_generation_begin(enrollment, &owner, true).ok() && !owner.record_present && owner.provisioning_admitted,
        "generation: only explicit provisioning permits missing first journal");
    if (!owner.gate) return;
    check(update_generation_begin(enrollment, &peer, true).error == UpdateGenerationError::gate && update_generation_release(&peer).ok(),
        "generation: competing installers cannot share the exclusive installation transaction");
    FixtureAuthority authority{true, 0, 0}; UpdateGenerationAuthenticator auth{&authority, fixture_authority};
    authority.accept = false;
    check(update_generation_commit(&owner, initial, auth).error == UpdateGenerationError::authenticity && !owner.record_present,
        "generation: a checksummed descriptor cannot publish without installed signed authenticity approval");
    authority.accept = true;
    check(update_generation_commit(&owner, initial, auth).ok() && update_generation_commit(&owner, prepared, auth).ok(),
        "generation: a flushed whole-pair journal advances through legal prepared publication");
    UpdatePairDescriptor selected{};
    check(update_generation_select(&owner, auth, &selected).ok() && update_generation_pair_equal(selected, initial.active.pair),
        "generation: prepared journal still selects only authenticated prior active files");
    check(update_generation_commit(&owner, pending, auth).ok() && update_generation_select(&owner, auth, &selected).error == UpdateGenerationError::state &&
        update_generation_release(&owner).ok() && update_generation_begin(enrollment, &owner).ok() && state_equal(owner.state, pending),
        "generation: process death after pending publication reopens the exact whole-pair transaction without synthesizing acknowledgement");
    authority.refused_digest = new_pair.document_sha256[0];
    check(update_generation_commit(&owner, rollback, auth).ok() && update_generation_select(&owner, auth, &selected).ok() &&
        update_generation_pair_equal(selected, initial.active.pair), "generation: corrupt pending candidate cannot veto independently authentic old-pair rollback");
    authority.refused_digest = 0;
    check(update_generation_commit(&owner, finished, auth).error == UpdateGenerationError::state &&
        dispose_candidate_fixture(owner, directory, auth) && state_equal(owner.state, finished),
        "generation: exact real file identities and durable disposal intent must precede rollback journal finalization");
    UpdateGenerationState retry_prepared{}; update_generation_prepare(finished, transaction, new_pair, &retry_prepared);
    update_generation_encode(retry_prepared, {record, kUpdateGenerationRecordBytes});
    check(write_new(temporary_path, record, kUpdateGenerationRecordBytes) &&
        update_generation_recover_temporary(&owner, auth).ok() && state_equal(owner.state, finished) &&
        GetFileAttributesW(temporary_path) == INVALID_FILE_ATTRIBUTES,
        "generation: interruption before record replacement discards only an authenticated legal temporary and keeps committed active");
    record[90] ^= 1;
    check(write_new(temporary_path, record, kUpdateGenerationRecordBytes) &&
        update_generation_recover_temporary(&owner, auth).error == UpdateGenerationError::checksum &&
        update_generation_cleanup_record(&owner).ok() && GetFileAttributesW(temporary_path) != INVALID_FILE_ATTRIBUTES,
        "generation: torn crash-left metadata is closed and left intact rather than claimed or deleted");
    // This fixture itself wrote the torn file and may remove that exact owner path.
    DeleteFileW(temporary_path);
    wchar_t foreign_path[MAX_PATH + 32]{}; append(root, L"\\unknown.fixture", foreign_path);
    const u8 foreign_bytes[1]{0x55};
    const bool foreign_created = write_new(foreign_path, foreign_bytes, sizeof(foreign_bytes));
    check(foreign_created && update_generation_directory_open(&owner, {new_pair.document_sha256, 32}, true, &directory).error == UpdateGenerationError::state &&
        !directory.directory && !directory.created && GetFileAttributesW(foreign_path) != INVALID_FILE_ATTRIBUTES,
        "generation: bounded root inventory refuses unknown or orphan entries without creating another candidate or deleting anything");
    if (foreign_created) DeleteFileW(foreign_path); // exact file written by this fixture
    wchar_t orphan_path[MAX_PATH + 80]{};
    u8 orphan_digest[32]{}; orphan_digest[0] = 0x99; u32 orphan_characters = 0;
    const bool orphan_named = update_generation_path(owner, {orphan_digest, 32}, UpdateGenerationFile::executable,
        {orphan_path, static_cast<u32>(sizeof(orphan_path) / sizeof(wchar_t))}, &orphan_characters).ok();
    if (orphan_named) {
        while (orphan_characters && orphan_path[orphan_characters - 1] != L'\\') --orphan_characters;
        if (orphan_characters) orphan_path[orphan_characters - 1] = 0;
    }
    const bool orphan_created = orphan_named && orphan_characters && CreateDirectoryW(orphan_path, nullptr);
    const auto orphan_refusal = update_generation_directory_open(&owner, {new_pair.document_sha256, 32}, true, &directory);
    const DWORD orphan_attributes = GetFileAttributesW(orphan_path);
    check(orphan_created && orphan_refusal.error ==
        UpdateGenerationError::state && !directory.directory && !directory.created &&
        orphan_attributes != INVALID_FILE_ATTRIBUTES && (orphan_attributes & FILE_ATTRIBUTE_DIRECTORY),
        "generation: crash-left unreferenced digest directory blocks another candidate without deletion");
    if (orphan_created) check(RemoveDirectoryW(orphan_path) != FALSE,
        "generation: fixture removes only its exact owned empty orphan after verifying refusal");
    UpdateGenerationDirectory unresolved{}; unresolved.transaction = &owner; unresolved.created = true;
    copy_bytes(unresolved.document_sha256, new_pair.document_sha256, 32);
    check(update_generation_directory_release(&unresolved, true).error == UpdateGenerationError::cleanup && !unresolved.directory,
        "generation: create-success open-failure cannot adopt an uncaptured directory identity for disposal");
    check(update_generation_directory_open(&owner, {new_pair.document_sha256, 32}, true, &directory).ok() &&
        update_generation_file_open(&directory, UpdateGenerationFile::executable, true, &writable).ok(),
        "generation: signed digest chooses the fixed exclusive candidate directory and exact member basename");
    if (!directory.directory || !writable) return;
    DWORD written = 0; const u8 payload[3]{1, 2, 3};
    const bool wrote = WriteFile(writable, payload, sizeof(payload), &written, nullptr) && written == sizeof(payload);
    check(wrote && update_generation_file_seal(&directory, &writable, &sealed).ok() && !writable && sealed,
        "generation: candidate file flush and access reduction retain explicit readonly disposal ownership");
    HANDLE duplicate_target = nullptr;
    check(update_generation_file_open(&directory, UpdateGenerationFile::executable, true, &duplicate_target).error == UpdateGenerationError::open && !duplicate_target &&
        update_generation_directory_release(&directory, true).error == UpdateGenerationError::cleanup && directory.directory,
        "generation: existing member is never overwritten and nonempty directory cleanup stays retryable without recursive deletion");
    DWORD denied_write = 0;
    check(!WriteFile(sealed, payload, sizeof(payload), &denied_write, nullptr), "generation: sealed candidate capability refuses writes");
    check(dispose_file(&sealed) && update_generation_directory_release(&directory, true).ok() && !directory.directory,
        "generation: exact file-owner disposal permits empty owned directory cleanup");
    check(update_generation_release(&owner).ok() && !owner.record_present && !owner.state.sequence && !owner.root_characters,
        "generation: completed release clears prior enrollment and journal before owner reuse");
}
} // namespace maicord

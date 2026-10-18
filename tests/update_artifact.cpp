#include "maicord/app/update_artifact.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kAbcSha256[32]{
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
constexpr u8 kMillionASha256[32]{
    0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
    0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0};

bool write_fixture(const wchar_t* path, const u8* data, u32 bytes, u32 repetitions = 1) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (u32 index = 0; ok && index < repetitions; ++index) {
        DWORD written = 0;
        ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    }
    return CloseHandle(file) && ok;
}

bool write_million_a(const wchar_t* path) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 block[kUpdateArtifactReadBufferBytes];
    for (auto& value : block) value = 'a';
    u32 remaining = 1000000;
    bool ok = true;
    while (ok && remaining) {
        const DWORD wanted = minimum(remaining, static_cast<u32>(sizeof(block)));
        DWORD written = 0;
        ok = WriteFile(file, block, wanted, &written, nullptr) && written == wanted;
        remaining -= written;
    }
    return CloseHandle(file) && ok;
}
} // namespace

void test_update_artifact(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    const DWORD directory_chars = GetTempPathW(MAX_PATH, directory);
    const bool created = directory_chars && directory_chars < MAX_PATH && GetTempFileNameW(directory, L"MUA", 0, path);
    check(created, "update artifact: isolated external temporary fixture is created");
    if (!created) return;
    struct DeleteFixture {
        const wchar_t* path;
        ~DeleteFixture() noexcept { DeleteFileW(path); }
    } remove{path};
    u32 path_chars = 0;
    while (path[path_chars]) ++path_chars;
    const ConstSlice<wchar_t> file_path{path, path_chars};
    constexpr u8 abc[]{'a', 'b', 'c'};
    UpdateManifest approved{};
    approved.version = 6;
    approved.artifact_bytes = sizeof(abc);
    copy_bytes(approved.artifact_sha256, kAbcSha256, sizeof(kAbcSha256));
    for (auto& byte : approved.manifest_sha256) byte = 0x55;
    UpdateArtifact artifact{};
    struct ReleaseArtifact {
        UpdateArtifact* artifact;
        ~ReleaseArtifact() noexcept { update_artifact_release(artifact); }
    } release{&artifact};
    check(write_fixture(path, abc, sizeof(abc)), "update artifact: known SHA-256 abc fixture is written");
    const auto verified = update_artifact_verify(file_path, approved, &artifact);
    check(verified.ok() && artifact.file && artifact.bytes == 3 && artifact.version == approved.version &&
        bytes_equal(artifact.sha256, kAbcSha256, 32) && bytes_equal(artifact.manifest_sha256, approved.manifest_sha256, 32),
        "update artifact: known hash admits owned file handle and exact approved manifest identity");
    if (!verified.ok()) return;
    u8 read_back[3]{};
    DWORD read = 0;
    check(ReadFile(artifact.file, read_back, sizeof(read_back), &read, nullptr) && read == 3 && bytes_equal(read_back, abc, 3),
        "update artifact: retained verified file is positioned at zero for staging handoff");
    const UpdateArtifact owned = artifact;
    check(update_artifact_verify(file_path, approved, &artifact).error == UpdateArtifactError::invalid_input &&
        bytes_equal(&artifact, &owned, sizeof(artifact)), "update artifact: an owned output handle cannot be overwritten or leaked");
    HANDLE peer_reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(peer_reader != INVALID_HANDLE_VALUE, "update artifact: verified ownership permits compatible readers");
    if (peer_reader != INVALID_HANDLE_VALUE) CloseHandle(peer_reader);
    HANDLE writer = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD writer_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    check(writer == INVALID_HANDLE_VALUE && writer_error == ERROR_SHARING_VIOLATION && !DeleteFileW(path) &&
        GetLastError() == ERROR_SHARING_VIOLATION,
        "update artifact: retained verified handle denies file writes and deletion before handoff");
    const HANDLE verified_handle = artifact.file;
    const UpdateArtifact empty{};
    Win32LargeInteger closed_size{};
    check(update_artifact_release(&artifact).ok() && bytes_equal(&artifact, &empty, sizeof(artifact)) &&
        !GetFileSizeEx(verified_handle, closed_size.native()) && GetLastError() == ERROR_INVALID_HANDLE &&
        update_artifact_release(&artifact).ok(), "update artifact: explicit release closes handle, clears identity and is idempotent");

    UpdateArtifact sentinel{};
    sentinel.bytes = 0xFEDCBA9876543210ull;
    sentinel.version = 99;
    for (auto& value : sentinel.sha256) value = 0xA5;
    for (auto& value : sentinel.manifest_sha256) value = 0x5A;
    artifact = sentinel;
    auto reject = [&](const UpdateManifest& offer, UpdateArtifactError expected) noexcept {
        const auto failed = update_artifact_verify(file_path, offer, &artifact);
        return failed.error == expected && bytes_equal(&artifact, &sentinel, sizeof(artifact));
    };
    auto wrong = approved;
    wrong.artifact_sha256[0] ^= 1;
    check(reject(wrong, UpdateArtifactError::digest) && write_fixture(path, abc, 3),
        "update artifact: wrong digest publishes no candidate hash and releases its read lock on failure");
    wrong = approved; wrong.artifact_bytes = 2;
    check(reject(wrong, UpdateArtifactError::size), "update artifact: extra file bytes fail exact expected size atomically");
    wrong.artifact_bytes = 4;
    check(reject(wrong, UpdateArtifactError::size), "update artifact: short file fails exact expected size atomically");
    check(write_fixture(path, abc, 2) && reject(approved, UpdateArtifactError::size) && write_fixture(path, abc, 3),
        "update artifact: truncated artifact is rejected before admitting any digest");
    wrong = approved; wrong.artifact_bytes = 0;
    check(reject(wrong, UpdateArtifactError::invalid_input), "update artifact: zero expected bytes cannot admit an empty artifact");
    wrong.artifact_bytes = kUpdateArtifactMaximumBytes + 1;
    check(reject(wrong, UpdateArtifactError::invalid_input), "update artifact: SHA-256 bit-length overflow is rejected before file IO");
    wrong.artifact_bytes = ~u64{0};
    check(reject(wrong, UpdateArtifactError::invalid_input), "update artifact: uint64 maximum cannot wrap expected byte accounting");
    wrong.artifact_bytes = kUpdateArtifactMaximumBytes;
    check(reject(wrong, UpdateArtifactError::size), "update artifact: exact SHA-256 maximum byte bound remains representable");
    wrong.artifact_bytes = (u64{1} << 32) + 3;
    check(reject(wrong, UpdateArtifactError::size), "update artifact: 64-bit expected size never narrows to a matching DWORD length");
    wrong.artifact_bytes = ~u32{0};
    check(reject(wrong, UpdateArtifactError::size), "update artifact: DWORD maximum is compared as a full 64-bit file size");
    wrong = approved; wrong.version = 0;
    check(reject(wrong, UpdateArtifactError::invalid_input), "update artifact: unversioned plan is refused without output publication");

    writer = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "update artifact: sharing-rejection fixture owns a live writer");
    if (writer != INVALID_HANDLE_VALUE) {
        const auto blocked = update_artifact_verify(file_path, approved, &artifact);
        check(blocked.error == UpdateArtifactError::open && blocked.native_error == ERROR_SHARING_VIOLATION &&
            bytes_equal(&artifact, &sentinel, sizeof(artifact)), "update artifact: an existing writer prevents a verified read-only handoff");
        CloseHandle(writer);
    }
    constexpr wchar_t relative[] = L"MaiCord-update.zip";
    constexpr wchar_t drive_relative[] = L"D:MaiCord-update.zip";
    constexpr wchar_t device[] = L"\\\\.\\MaiCord-update.zip";
    constexpr wchar_t embedded[] = L"D:\\Mai\0Cord-update.zip";
    const auto path_rejected = [&](ConstSlice<wchar_t> value) noexcept {
        return update_artifact_verify(value, approved, &artifact).error == UpdateArtifactError::path &&
            bytes_equal(&artifact, &sentinel, sizeof(artifact));
    };
    check(path_rejected({relative, sizeof(relative) / sizeof(relative[0]) - 1}) &&
        path_rejected({drive_relative, sizeof(drive_relative) / sizeof(drive_relative[0]) - 1}) &&
        path_rejected({device, sizeof(device) / sizeof(device[0]) - 1}) &&
        path_rejected({embedded, sizeof(embedded) / sizeof(embedded[0]) - 1}),
        "update artifact: relative, drive-relative, device and embedded-NUL paths fail before opening");
    check(update_artifact_verify({}, approved, &artifact).error == UpdateArtifactError::invalid_input &&
        update_artifact_verify({path, kUpdateArtifactMaximumPathCharacters + 1}, approved, &artifact).error == UpdateArtifactError::invalid_input &&
        update_artifact_verify(file_path, approved, nullptr).error == UpdateArtifactError::invalid_input &&
        update_artifact_release(nullptr).error == UpdateArtifactError::invalid_input &&
        update_artifact_verify({reinterpret_cast<const wchar_t*>(~usize{0} - 1), 3}, approved, &artifact).error == UpdateArtifactError::invalid_input &&
        bytes_equal(&artifact, &sentinel, sizeof(artifact)), "update artifact: null, oversized and wrapping arguments fail atomically");
    const UpdateManifest original_plan = approved;
    check(update_artifact_verify(file_path, approved, reinterpret_cast<UpdateArtifact*>(&approved)).error ==
        UpdateArtifactError::invalid_input && bytes_equal(&approved, &original_plan, sizeof(approved)),
        "update artifact: output cannot alias approved manifest identity or expectations");
    check(update_artifact_verify({reinterpret_cast<const wchar_t*>(&artifact), 3}, approved, &artifact).error ==
        UpdateArtifactError::invalid_input && bytes_equal(&artifact, &sentinel, sizeof(artifact)),
        "update artifact: output cannot alias a borrowed path");

    check(write_million_a(path), "update artifact: million-byte known-answer fixture is written with bounded storage");
    approved.artifact_bytes = 1000000;
    copy_bytes(approved.artifact_sha256, kMillionASha256, sizeof(kMillionASha256));
    check(update_artifact_verify(file_path, approved, &artifact).ok() && artifact.bytes == 1000000 &&
        bytes_equal(artifact.sha256, kMillionASha256, 32) && update_artifact_release(&artifact).ok(),
        "update artifact: repeated fixed-buffer reads and final partial chunk match the million-a SHA-256 known answer");
    artifact = sentinel;
    check(DeleteFileW(path), "update artifact: success release permits external fixture deletion");
    const auto missing = update_artifact_verify(file_path, approved, &artifact);
    check(missing.error == UpdateArtifactError::open && missing.native_error == ERROR_FILE_NOT_FOUND &&
        bytes_equal(&artifact, &sentinel, sizeof(artifact)), "update artifact: missing external file reports native error with no hash publication");
}
} // namespace maicord

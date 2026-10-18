#include "maicord/app/update_stage.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 kStageAbcDigest[32]{
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
constexpr u8 kStageMillionDigest[32]{
    0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
    0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0};
u32 text_count(const wchar_t* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
void child_path(wchar_t* output, const wchar_t* parent, const wchar_t* name) noexcept {
    u32 count = 0;
    while (parent[count]) { output[count] = parent[count]; ++count; }
    output[count++] = L'\\';
    for (u32 index = 0; name[index]; ++index) output[count++] = name[index];
    output[count] = 0;
}
bool stage_fixture(const wchar_t* path, bool million = false) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 block[kUpdateArtifactReadBufferBytes];
    for (auto& value : block) value = 'a';
    if (!million) { block[1] = 'b'; block[2] = 'c'; }
    u32 remaining = million ? 1000000 : 3;
    bool ok = true;
    while (remaining && ok) {
        DWORD written = 0;
        const DWORD wanted = minimum(remaining, static_cast<u32>(sizeof(block)));
        ok = WriteFile(file, block, wanted, &written, nullptr) && written == wanted;
        remaining -= written;
    }
    return CloseHandle(file) && ok;
}
bool stage_read_abc(HANDLE file) noexcept {
    u8 bytes[4]{};
    DWORD read = 0;
    LARGE_INTEGER zero{};
    constexpr u8 abc[]{'a','b','c'};
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && ReadFile(file, bytes, sizeof(bytes), &read, nullptr) &&
        read == 3 && bytes_equal(bytes, abc, 3);
}
} // namespace

void test_update_stage(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t temp[MAX_PATH]{}, directory[MAX_PATH]{}, source_path[MAX_PATH]{}, staged_path[MAX_PATH]{};
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    const bool reserved = count && count < MAX_PATH && GetTempFileNameW(temp, L"MUS", 0, directory);
    check(reserved, "update stage: reserve isolated external owned temporary path");
    if (!reserved) return;
    const bool created = DeleteFileW(directory) && CreateDirectoryW(directory, nullptr);
    check(created, "update stage: create owned isolated external directory");
    if (!created) return;
    child_path(source_path, directory, L"source.bin");
    child_path(staged_path, directory, L"package.stage");
    struct Fixtures {
        const wchar_t* directory;
        const wchar_t* source;
        const wchar_t* stage;
        bool source_owned = false;
        bool stage_owned = false;
        ~Fixtures() noexcept {
            if (stage_owned) DeleteFileW(stage);
            if (source_owned) DeleteFileW(source);
            RemoveDirectoryW(directory);
        }
    } fixtures{directory, source_path, staged_path};
    UpdateManifest approved{};
    approved.version = 6;
    approved.artifact_bytes = 3;
    copy_bytes(approved.artifact_sha256, kStageAbcDigest, 32);
    for (auto& value : approved.manifest_sha256) value = 0x55;
    constexpr char signed_name[] = "MaiCord-6.exe";
    copy_bytes(approved.artifact_name, signed_name, sizeof(signed_name));
    approved.artifact_name_bytes = sizeof(signed_name) - 1;
    constexpr wchar_t filename[] = L"package.stage";
    const ConstSlice<wchar_t> parent{directory, text_count(directory)};
    const ConstSlice<wchar_t> name{filename, sizeof(filename) / sizeof(filename[0]) - 1};
    const ConstSlice<wchar_t> source_name{source_path, text_count(source_path)};
    UpdateArtifact source{};
    UpdateStage stage{};
    struct Ownership {
        UpdateArtifact* source;
        UpdateStage* stage;
        ~Ownership() noexcept { update_stage_release(stage, true); update_artifact_release(source); }
    } ownership{&source, &stage};
    fixtures.source_owned = stage_fixture(source_path);
    check(fixtures.source_owned && update_artifact_verify(source_name, approved, &source).ok(),
        "update stage: verified retained abc source fixture");
    if (!source.file) return;
    const auto copied = update_stage_copy(&source, approved, parent, name, &stage);
    fixtures.stage_owned = copied.ok();
    check(copied.ok() && stage.file && stage.bytes == 3 && stage.version == 6 &&
        bytes_equal(stage.sha256, kStageAbcDigest, 32) && bytes_equal(stage.manifest_sha256, approved.manifest_sha256, 32) &&
        stage.artifact_name_bytes == approved.artifact_name_bytes &&
        bytes_equal(stage.artifact_name, signed_name, sizeof(signed_name)) && stage_read_abc(stage.file),
        "update stage: durable exact copy publishes retained digest, document identity and signed artifact name");
    if (!stage.file) return;
    DWORD written = 0;
    constexpr u8 changed = 'x';
    check(!WriteFile(stage.file, &changed, 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
        "update stage: published handle has no write access");
    HANDLE peer = CreateFileW(staged_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    check(peer != INVALID_HANDLE_VALUE && stage_read_abc(peer), "update stage: retained file permits compatible read-only peers");
    if (peer != INVALID_HANDLE_VALUE) CloseHandle(peer);
    HANDLE writer = CreateFileW(staged_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD writer_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    check(writer == INVALID_HANDLE_VALUE && writer_error == ERROR_SHARING_VIOLATION &&
        !DeleteFileW(staged_path) && GetLastError() == ERROR_SHARING_VIOLATION,
        "update stage: reduced duplicate keeps writer/deletion sharing denied");
    const UpdateStage snapshot = stage;
    check(update_stage_copy(&source, approved, parent, name, &stage).error == UpdateStageError::invalid_input &&
        bytes_equal(&stage, &snapshot, sizeof(stage)), "update stage: owned output is never overwritten");
    check(update_stage_release(&stage, false).ok() && !stage.file && source.file,
        "update stage: preserve release clears ownership while leaving source reusable");
    const auto exists = update_stage_copy(&source, approved, parent, name, &stage);
    peer = CreateFileW(staged_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(exists.error == UpdateStageError::create && exists.native_error == ERROR_FILE_EXISTS && !stage.file &&
        peer != INVALID_HANDLE_VALUE && stage_read_abc(peer), "update stage: CREATE_NEW preserves previously staged destination bytes");
    if (peer != INVALID_HANDLE_VALUE) CloseHandle(peer);
    check(DeleteFileW(staged_path), "update stage: remove only previously created and released test stage");
    fixtures.stage_owned = false;

    // Real Win32 UTF-16 parent: leading dot, BMP letters, internal space and a
    // supplementary scalar. Signed artifact and stage filenames stay ASCII.
    constexpr wchar_t unicode_component[] = L".\u017d\u7528\u6237 \U0001f600";
    const bool unicode_room = text_count(directory) + text_count(unicode_component) + 15 < MAX_PATH;
    check(unicode_room, "update stage: bounded Unicode directory fixture path fits fixed storage");
    if (unicode_room) {
        wchar_t unicode_directory[MAX_PATH]{}, unicode_source_path[MAX_PATH]{}, unicode_stage_path[MAX_PATH]{};
        child_path(unicode_directory, directory, unicode_component);
        const bool unicode_created = CreateDirectoryW(unicode_directory, nullptr) != FALSE;
        check(unicode_created, "update stage: owned Unicode parent directory is created on disk");
        if (unicode_created) {
            child_path(unicode_source_path, unicode_directory, L"source.bin");
            child_path(unicode_stage_path, unicode_directory, filename);
            Fixtures unicode_fixtures{unicode_directory, unicode_source_path, unicode_stage_path};
            UpdateArtifact unicode_source{};
            UpdateStage unicode_stage{};
            Ownership unicode_ownership{&unicode_source, &unicode_stage};
            unicode_fixtures.source_owned = stage_fixture(unicode_source_path);
            const bool unicode_verified = unicode_fixtures.source_owned && update_artifact_verify(
                {unicode_source_path, text_count(unicode_source_path)}, approved, &unicode_source).ok();
            auto unicode_copy = unicode_verified ? update_stage_copy(&unicode_source, approved,
                {unicode_directory, text_count(unicode_directory)}, name, &unicode_stage) : UpdateStageResult{};
            unicode_fixtures.stage_owned = unicode_stage.file != nullptr;
            check(unicode_verified && unicode_copy.ok() && unicode_stage.file && stage_read_abc(unicode_stage.file) &&
                bytes_equal(unicode_stage.artifact_name, signed_name, sizeof(signed_name)),
                "update stage: real Unicode parent supports retained artifact verification and durable ASCII-named copy");
            check(update_stage_cleanup_release(&unicode_copy).ok() && update_stage_release(&unicode_stage, true).ok() &&
                update_artifact_release(&unicode_source).ok() && GetFileAttributesW(unicode_stage_path) == INVALID_FILE_ATTRIBUTES &&
                GetLastError() == ERROR_FILE_NOT_FOUND,
                "update stage: Unicode stage is disposed through its retained handle");
            unicode_fixtures.stage_owned = false;
        }
    }

    UpdateStage sentinel{};
    sentinel.bytes = ~u64{0};
    sentinel.version = 99;
    stage = sentinel;
    auto rejection = [&](const UpdateManifest& plan, UpdateStageError error) noexcept {
        return update_stage_copy(&source, plan, parent, name, &stage).error == error &&
            bytes_equal(&stage, &sentinel, sizeof(stage));
    };
    UpdateManifest wrong = approved;
    wrong.version += 1;
    check(rejection(wrong, UpdateStageError::identity), "update stage: signed version identity must match retained artifact");
    wrong = approved; wrong.manifest_sha256[0] ^= 1;
    check(rejection(wrong, UpdateStageError::identity), "update stage: exact admitted document digest must match retained artifact");
    wrong = approved; wrong.artifact_sha256[0] ^= 1;
    check(rejection(wrong, UpdateStageError::identity), "update stage: artifact digest identity must match approved plan");
    wrong = approved; wrong.artifact_bytes = (u64{1} << 32) + 3;
    source.bytes = wrong.artifact_bytes;
    check(rejection(wrong, UpdateStageError::size), "update stage: full 64-bit size comparison cannot narrow to matching DWORD");
    source.bytes = approved.artifact_bytes;
    wrong = approved; wrong.artifact_bytes = kUpdateArtifactMaximumBytes + 1;
    check(rejection(wrong, UpdateStageError::invalid_input), "update stage: SHA-256 length overflow refused before copying");
    check(update_stage_copy(&source, approved, parent, name, reinterpret_cast<UpdateStage*>(&source)).error == UpdateStageError::invalid_input &&
        update_stage_copy(&source, approved, parent, {reinterpret_cast<const wchar_t*>(&stage), 3}, &stage).error == UpdateStageError::invalid_input &&
        update_stage_copy(&source, approved, {reinterpret_cast<const wchar_t*>(~usize{0} - 1), 3}, name, &stage).error == UpdateStageError::invalid_input &&
        update_stage_copy(nullptr, approved, parent, name, &stage).error == UpdateStageError::invalid_input &&
        bytes_equal(&stage, &sentinel, sizeof(stage)), "update stage: aliases, wrapping ranges and null inputs fail without output mutation");
    constexpr wchar_t traversal[] = L"..\\package.stage", executable[] = L"MaiCord.exe", stream[] = L"package.stage:evil";
    constexpr wchar_t relative[] = L"relative", device[] = L"\\\\.\\C:";
    check(update_stage_copy(&source, approved, parent, {traversal, sizeof(traversal) / sizeof(wchar_t) - 1}, &stage).error == UpdateStageError::path &&
        update_stage_copy(&source, approved, parent, {executable, sizeof(executable) / sizeof(wchar_t) - 1}, &stage).error == UpdateStageError::path &&
        update_stage_copy(&source, approved, parent, {stream, sizeof(stream) / sizeof(wchar_t) - 1}, &stage).error == UpdateStageError::path &&
        update_stage_copy(&source, approved, {relative, sizeof(relative) / sizeof(wchar_t) - 1}, name, &stage).error == UpdateStageError::path &&
        update_stage_copy(&source, approved, {device, sizeof(device) / sizeof(wchar_t) - 1}, name, &stage).error == UpdateStageError::path,
        "update stage: traversal, executable target, ADS, relative and device paths refused");
    const auto bad_parent = [&](ConstSlice<wchar_t> value) noexcept {
        return update_stage_copy(&source, approved, value, name, &stage).error == UpdateStageError::path &&
            bytes_equal(&stage, &sentinel, sizeof(stage));
    };
    constexpr wchar_t high_surrogate[] = L"D:\\\xD800", low_surrogate[] = L"D:\\\xDC00";
    constexpr wchar_t mismatched_surrogate[] = L"D:\\\xD800-";
    constexpr wchar_t reserved_superscript[] = L"D:\\COM\u00b9", reserved_extension[] = L"D:\\lpt\u00b2.data";
    constexpr wchar_t console[] = L"D:\\CONOUT$", control[] = L"D:\\bad\x0085name";
    constexpr wchar_t trailing_dot[] = L"D:\\folder.", trailing_space[] = L"D:\\folder ";
    check(bad_parent({high_surrogate, sizeof(high_surrogate) / sizeof(wchar_t) - 1}) &&
        bad_parent({low_surrogate, sizeof(low_surrogate) / sizeof(wchar_t) - 1}) &&
        bad_parent({mismatched_surrogate, sizeof(mismatched_surrogate) / sizeof(wchar_t) - 1}) &&
        bad_parent({reserved_superscript, sizeof(reserved_superscript) / sizeof(wchar_t) - 1}) &&
        bad_parent({reserved_extension, sizeof(reserved_extension) / sizeof(wchar_t) - 1}) &&
        bad_parent({console, sizeof(console) / sizeof(wchar_t) - 1}) &&
        bad_parent({control, sizeof(control) / sizeof(wchar_t) - 1}) &&
        bad_parent({trailing_dot, sizeof(trailing_dot) / sizeof(wchar_t) - 1}) &&
        bad_parent({trailing_space, sizeof(trailing_space) / sizeof(wchar_t) - 1}),
        "update stage: malformed UTF-16, superscript devices, console aliases, controls and ambiguous trailing characters fail before IO");
    constexpr wchar_t unicode_filename[] = L"package\u017d.stage";
    check(update_stage_copy(&source, approved, parent,
        {unicode_filename, sizeof(unicode_filename) / sizeof(wchar_t) - 1}, &stage).error == UpdateStageError::path,
        "update stage: Unicode parent support does not widen the approved ASCII stage filename policy");

    check(update_artifact_release(&source).ok(), "update stage: release source before creating writable mapping race fixture");
    HANDLE mapping_file = CreateFileW(source_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE mapping = mapping_file == INVALID_HANDLE_VALUE ? nullptr : CreateFileMappingW(mapping_file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    u8* view = mapping ? static_cast<u8*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0)) : nullptr;
    if (mapping) CloseHandle(mapping);
    if (mapping_file != INVALID_HANDLE_VALUE) CloseHandle(mapping_file);
    const auto mapped_verify = update_artifact_verify(source_name, approved, &source);
    check(view != nullptr && (mapped_verify.ok() || (mapped_verify.error == UpdateArtifactError::open &&
        mapped_verify.native_error == ERROR_SHARING_VIOLATION)),
        "update stage: writable view either permits retained verification or is rejected by the OS sharing gate");
    if (view && !source.file) {
        // Negative primitive test only: a synthetic source identity is not an
        // authenticity gate. Accommodate filesystems that reject verification
        // while a writable mapping exists, then still exercise the copy hash.
        HANDLE read_source = CreateFileW(source_path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (read_source != INVALID_HANDLE_VALUE) {
            source.file = read_source;
            source.bytes = approved.artifact_bytes;
            source.version = approved.version;
            copy_bytes(source.sha256, approved.artifact_sha256, 32);
            copy_bytes(source.manifest_sha256, approved.manifest_sha256, 32);
        }
        check(source.file != nullptr, "update stage: negative copy-recheck fixture retains a read-only source handle");
    }
    if (view && source.file) {
        view[0] = 'x';
        check(FlushViewOfFile(view, 3) && rejection(approved, UpdateStageError::digest) &&
            GetFileAttributesW(staged_path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
            "update stage: copy rehash rejects stale bytes changed through writable mapping and removes owned partial stage");
        view[0] = 'a';
        check(FlushViewOfFile(view, 3), "update stage: restore owned mapping fixture");
    }
    if (view) UnmapViewOfFile(view);
    update_artifact_release(&source);
    check(DeleteFileW(source_path), "update stage: release and delete only owned source fixture before bounded large copy");
    fixtures.source_owned = false;
    fixtures.source_owned = stage_fixture(source_path, true);
    approved.artifact_bytes = 1000000;
    copy_bytes(approved.artifact_sha256, kStageMillionDigest, 32);
    check(fixtures.source_owned && update_artifact_verify(source_name, approved, &source).ok() &&
        update_stage_copy(&source, approved, parent, name, &stage).ok() && stage.bytes == 1000000 &&
        bytes_equal(stage.sha256, kStageMillionDigest, 32), "update stage: million-a known hash exercises bounded repeated reads and final partial buffer");
    fixtures.stage_owned = stage.file != nullptr;
    const HANDLE held = stage.file;
    LARGE_INTEGER held_size{};
    check(update_stage_release(&stage, true).ok() && !stage.file &&
        (!held || (!GetFileSizeEx(held, &held_size) && GetLastError() == ERROR_INVALID_HANDLE)) &&
        GetFileAttributesW(staged_path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND &&
        update_stage_release(&stage, true).ok() && update_stage_release(nullptr, true).error == UpdateStageError::invalid_input,
        "update stage: explicit handle-based discard closes ownership, deletes staged file and is idempotent");
    fixtures.stage_owned = false;
}
} // namespace maicord

#include "maicord/app/update_artifact.hpp"
#include "maicord/core/byte_range.hpp"
#include <Windows.h>
#include <bcrypt.h>

namespace maicord {
namespace {
UpdateArtifactResult result(UpdateArtifactError error, DWORD native = 0, i32 crypto = 0) noexcept {
    return {error, native, crypto};
}
bool valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || !path.count || path.count > kUpdateArtifactMaximumPathCharacters) return false;
    for (u32 index = 0; index < path.count; ++index)
        if (!path[index] || path[index] < 32 || path[index] == L'/' || path[index] == L'*' || path[index] == L'?') return false;
    const wchar_t drive = path[0] >= L'A' && path[0] <= L'Z' ?
        static_cast<wchar_t>(path[0] + L'a' - L'A') : path[0];
    if (path.count > 3 && drive >= L'a' && drive <= L'z' && path[1] == L':' && path[2] == L'\\') {
        for (u32 index = 3; index < path.count; ++index) if (path[index] == L':') return false;
        return true;
    }
    if (path.count < 6 || path[0] != L'\\' || path[1] != L'\\' || path[2] == L'.' || path[2] == L'\\') return false;
    u32 server_end = 2;
    while (server_end < path.count && path[server_end] != L'\\') ++server_end;
    const u32 share_begin = server_end + 1;
    if (share_begin >= path.count || path[share_begin] == L'\\') return false;
    u32 share_end = share_begin;
    while (share_end < path.count && path[share_end] != L'\\') ++share_end;
    if (share_end + 1 >= path.count) return false;
    for (u32 index = 2; index < path.count; ++index) if (path[index] == L':') return false;
    return true;
}
struct Temporary {
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_HASH_HANDLE hash = nullptr;
    u8 digest[32]{};
    ~Temporary() noexcept {
        if (hash) BCryptDestroyHash(hash);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        SecureZeroMemory(digest, sizeof(digest));
    }
};
} // namespace

UpdateArtifactResult update_artifact_verify(ConstSlice<wchar_t> absolute_path,
    const UpdateManifest& approved, UpdateArtifact* output) noexcept {
    if (!output || !absolute_path.data || !absolute_path.count ||
        absolute_path.count > kUpdateArtifactMaximumPathCharacters || !approved.version ||
        !approved.artifact_bytes || approved.artifact_bytes > kUpdateArtifactMaximumBytes)
        return result(UpdateArtifactError::invalid_input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(absolute_path.data), absolute_path.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(&approved), sizeof(approved)},
        {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    for (u32 index = 0; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
        if (!byte_range_valid(ranges[index])) return result(UpdateArtifactError::invalid_input);
        for (u32 previous = 0; previous < index; ++previous)
            if (byte_ranges_overlap(ranges[index], ranges[previous])) return result(UpdateArtifactError::invalid_input);
    }
    if (output->file) return result(UpdateArtifactError::invalid_input);
    if (!valid_path(absolute_path)) return result(UpdateArtifactError::path);
    wchar_t terminated[kUpdateArtifactMaximumPathCharacters + 1];
    for (u32 index = 0; index < absolute_path.count; ++index) terminated[index] = absolute_path[index];
    terminated[absolute_path.count] = 0;
    Temporary temporary{};
    temporary.file = CreateFileW(terminated, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (temporary.file == INVALID_HANDLE_VALUE) return result(UpdateArtifactError::open, GetLastError());
    if (GetFileType(temporary.file) != FILE_TYPE_DISK) return result(UpdateArtifactError::file);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(temporary.file, &information)) return result(UpdateArtifactError::file, GetLastError());
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return result(UpdateArtifactError::file);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(temporary.file, &size)) return result(UpdateArtifactError::file, GetLastError());
    if (size.QuadPart < 0 || static_cast<u64>(size.QuadPart) != approved.artifact_bytes)
        return result(UpdateArtifactError::size);
    NTSTATUS status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &temporary.hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    u8 buffer[kUpdateArtifactReadBufferBytes];
    u64 remaining = approved.artifact_bytes;
    while (remaining) {
        const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(temporary.file, buffer, wanted, &read, nullptr)) return result(UpdateArtifactError::read, GetLastError());
        if (!read || read > wanted) return result(UpdateArtifactError::size);
        status = BCryptHashData(temporary.hash, buffer, read, 0);
        if (status < 0) return result(UpdateArtifactError::hash, 0, status);
        remaining -= read;
    }
    DWORD trailing = 0;
    if (!ReadFile(temporary.file, buffer, 1, &trailing, nullptr)) return result(UpdateArtifactError::read, GetLastError());
    if (trailing) return result(UpdateArtifactError::size);
    status = BCryptFinishHash(temporary.hash, temporary.digest, sizeof(temporary.digest), 0);
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    if (!bytes_equal(temporary.digest, approved.artifact_sha256, sizeof(temporary.digest)))
        return result(UpdateArtifactError::digest);
    status = BCryptDestroyHash(temporary.hash);
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    temporary.hash = nullptr;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(temporary.file, zero, nullptr, FILE_BEGIN)) return result(UpdateArtifactError::read, GetLastError());
    UpdateArtifact candidate{};
    candidate.file = temporary.file;
    candidate.bytes = approved.artifact_bytes;
    candidate.version = approved.version;
    copy_bytes(candidate.sha256, temporary.digest, sizeof(candidate.sha256));
    copy_bytes(candidate.manifest_sha256, approved.manifest_sha256, sizeof(candidate.manifest_sha256));
    *output = candidate;
    temporary.file = INVALID_HANDLE_VALUE;
    return result(UpdateArtifactError::none);
}

UpdateArtifactResult update_artifact_release(UpdateArtifact* artifact) noexcept {
    if (!artifact) return result(UpdateArtifactError::invalid_input);
    if (artifact->file && !CloseHandle(artifact->file)) return result(UpdateArtifactError::release, GetLastError());
    SecureZeroMemory(artifact, sizeof(*artifact));
    return result(UpdateArtifactError::none);
}
} // namespace maicord

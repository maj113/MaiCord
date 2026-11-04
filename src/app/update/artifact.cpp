#include "maicord/app/update/artifact.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"
#include "maicord/os/file.hpp"

namespace maicord {
namespace {
UpdateArtifactResult result(UpdateArtifactError error, DWORD native = 0, i32 crypto = 0) noexcept {
    return {error, native, crypto};
}
// An absolute path the artifact check accepts: a drive or UNC path on Windows, a rooted one elsewhere. Wildcards and
// control characters never pass.
bool valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || !path.count || path.count > kUpdateArtifactMaximumPathCharacters) return false;
    for (u32 index = 0; index < path.count; ++index)
        if (!path[index] || path[index] < 32 || path[index] == L'*' || path[index] == L'?') return false;
#if defined(_WIN32)
    for (u32 index = 0; index < path.count; ++index)
        if (path[index] == L'/') return false;
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
#else
    for (u32 index = 0; index < path.count; ++index)
        if (path[index] == L'\\') return false;
    return path.count > 1 && path[0] == L'/';
#endif
}
struct Temporary {
    HANDLE file = INVALID_HANDLE_VALUE;
    HashStream hash = nullptr;
    u8 digest[32]{};
    ~Temporary() noexcept {
        if (hash) hash_stream_destroy(hash);
        if (file != INVALID_HANDLE_VALUE) file_close(file);
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
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])}) || output->file)
        return result(UpdateArtifactError::invalid_input);
    if (!valid_path(absolute_path)) return result(UpdateArtifactError::path);
    wchar_t terminated[kUpdateArtifactMaximumPathCharacters + 1];
    for (u32 index = 0; index < absolute_path.count; ++index) terminated[index] = absolute_path[index];
    terminated[absolute_path.count] = 0;
    Temporary temporary{};
    temporary.file = file_open(terminated, {FileAccess::read, FileCreate::existing, kFileShareRead,
        kFileSequential | kFileNoFollow});
    if (temporary.file == INVALID_HANDLE_VALUE) return result(UpdateArtifactError::open, thread_last_error());
    FileStat information{};
    if (!file_stat(temporary.file, &information)) return result(UpdateArtifactError::file, thread_last_error());
    if (information.kind != FileKind::regular) return result(UpdateArtifactError::file);
    if (information.bytes != approved.artifact_bytes)
        return result(UpdateArtifactError::size);
    i32 status = hash_stream_begin(&temporary.hash, HashAlgorithm::sha256);
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    u8 buffer[kUpdateArtifactReadBufferBytes];
    u64 remaining = approved.artifact_bytes;
    while (remaining) {
        const u32 wanted = static_cast<u32>(minimum<u64>(remaining, sizeof(buffer)));
        u32 read = 0;
        if (!file_read(temporary.file, buffer, wanted, &read)) return result(UpdateArtifactError::read, thread_last_error());
        if (!read || read > wanted) return result(UpdateArtifactError::size);
        status = hash_stream_update(temporary.hash, buffer, read);
        if (status < 0) return result(UpdateArtifactError::hash, 0, status);
        remaining -= read;
    }
    u32 trailing = 0;
    if (!file_read(temporary.file, buffer, 1, &trailing)) return result(UpdateArtifactError::read, thread_last_error());
    if (trailing) return result(UpdateArtifactError::size);
    status = hash_stream_finish(temporary.hash, temporary.digest, sizeof(temporary.digest));
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    if (!bytes_equal(temporary.digest, approved.artifact_sha256, sizeof(temporary.digest)))
        return result(UpdateArtifactError::digest);
    status = hash_stream_destroy(temporary.hash);
    if (status < 0) return result(UpdateArtifactError::hash, 0, status);
    temporary.hash = nullptr;
    if (!file_seek(temporary.file, 0, FileSeek::begin)) return result(UpdateArtifactError::read, thread_last_error());
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
    if (artifact->file && !file_close(artifact->file)) return result(UpdateArtifactError::release, thread_last_error());
    SecureZeroMemory(artifact, sizeof(*artifact));
    return result(UpdateArtifactError::none);
}
} // namespace maicord

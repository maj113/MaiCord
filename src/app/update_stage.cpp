#include "maicord/app/update_stage.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {
UpdateStageResult result(UpdateStageError error, DWORD native = 0, i32 crypto = 0) noexcept {
    return {error, native, crypto, 0, nullptr, nullptr, nullptr, nullptr};
}
wchar_t lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + L'a' - L'A') : value;
}
bool component(ConstSlice<wchar_t> name) noexcept {
    if (!name.count || name[name.count - 1] == L'.' || name[name.count - 1] == L' ') return false;
    u32 stem = name.count;
    for (u32 index = 0; index < name.count; ++index) {
        const wchar_t value = name[index];
        if (value < 32 || (value >= 0x7f && value <= 0x9f) || value == L'\\' || value == L'/' || value == L':' ||
            value == L'*' || value == L'?' || value == L'"' || value == L'<' || value == L'>' || value == L'|') return false;
        if (stem == name.count && value == L'.') stem = index;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (index + 1 == name.count || name[index + 1] < 0xdc00 || name[index + 1] > 0xdfff) return false;
            ++index;
        } else if (value >= 0xdc00 && value <= 0xdfff) return false;
    }
    while (stem && name[stem - 1] == L' ') --stem;
    if (stem == 3) {
        const wchar_t a = lower(name[0]), b = lower(name[1]), c = lower(name[2]);
        if ((a == L'c' && b == L'o' && c == L'n') || (a == L'p' && b == L'r' && c == L'n') ||
            (a == L'a' && b == L'u' && c == L'x') || (a == L'n' && b == L'u' && c == L'l')) return false;
    }
    if (stem == 4 && ((name[3] >= L'0' && name[3] <= L'9') ||
        name[3] == 0xb9 || name[3] == 0xb2 || name[3] == 0xb3)) {
        const wchar_t a = lower(name[0]), b = lower(name[1]), c = lower(name[2]);
        if ((a == L'c' && b == L'o' && c == L'm') || (a == L'l' && b == L'p' && c == L't')) return false;
    }
    if ((stem == 6 || stem == 7) && lower(name[0]) == L'c' && lower(name[1]) == L'o' && lower(name[2]) == L'n') {
        if (stem == 6 && lower(name[3]) == L'i' && lower(name[4]) == L'n' && name[5] == L'$') return false;
        if (stem == 7 && lower(name[3]) == L'o' && lower(name[4]) == L'u' && lower(name[5]) == L't' && name[6] == L'$') return false;
    }
    return true;
}
bool parent_path(ConstSlice<wchar_t> path) noexcept {
    u32 start = 0;
    if (path.count >= 3 && lower(path[0]) >= L'a' && lower(path[0]) <= L'z' &&
        path[1] == L':' && path[2] == L'\\') start = 3;
    else if (path.count >= 5 && path[0] == L'\\' && path[1] == L'\\') start = 2;
    else return false;
    // Root drive is allowed; UNC needs at least a server and share component.
    u32 components = 0;
    for (u32 index = start; index <= path.count; ++index) {
        if (index != path.count && path[index] != L'\\') continue;
        if (!component({path.data + start, index - start})) return index == path.count && start == 3 && path.count == 3;
        ++components;
        start = index + 1;
    }
    return path[1] == L':' || components >= 2;
}
bool stage_name(ConstSlice<wchar_t> name) noexcept {
    if (!component(name) || name.count <= 6) return false;
    // Plan-derived ASCII filename only; exclude reserved console aliases and
    // shell metacharacters without imposing the filename whitelist on parents.
    for (u32 index = 0; index < name.count; ++index) {
        const wchar_t value = lower(name[index]);
        if ((value < L'a' || value > L'z') && (value < L'0' || value > L'9') &&
            value != L'.' && value != L'-' && value != L'_') return false;
    }
    constexpr wchar_t suffix[] = L".stage";
    for (u32 index = 0; index < 6; ++index)
        if (lower(name[name.count - 6 + index]) != suffix[index]) return false;
    return true;
}
bool same_file(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow;
}
struct CopyState {
    HANDLE parent = INVALID_HANDLE_VALUE;
    HANDLE destination = INVALID_HANDLE_VALUE;
    HANDLE retained = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    u8 digest[32]{};
    UpdateStageResult finish(UpdateStageResult status) noexcept {
        if (hash) {
            const NTSTATUS destroyed = BCryptDestroyHash(hash);
            if (destroyed < 0) {
                if (status.ok()) status = result(UpdateStageError::hash, 0, destroyed);
                status.crypto_status = destroyed;
                status.cleanup_hash = hash;
            }
            hash = nullptr;
        }
        if (destination != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(destination, FileDispositionInfo, &disposition, sizeof(disposition))) {
                status.cleanup_error = GetLastError();
                status.cleanup_file = destination;
            } else if (!CloseHandle(destination)) {
                status.cleanup_error = GetLastError();
                status.cleanup_file = destination;
            }
            destination = INVALID_HANDLE_VALUE;
        }
        if (retained && !CloseHandle(retained)) {
            if (!status.cleanup_error) status.cleanup_error = GetLastError();
            status.cleanup_duplicate = retained;
        }
        retained = nullptr;
        if (parent != INVALID_HANDLE_VALUE && !CloseHandle(parent)) {
            if (!status.cleanup_error) status.cleanup_error = GetLastError();
            status.cleanup_parent = parent;
        }
        parent = INVALID_HANDLE_VALUE;
        SecureZeroMemory(digest, sizeof(digest));
        return status;
    }
};
} // namespace

UpdateStageResult update_stage_copy(UpdateArtifact* source, const UpdateManifest& approved,
    ConstSlice<wchar_t> parent, ConstSlice<wchar_t> name, UpdateStage* output) noexcept {
    if (!source || !output || !parent.data || !parent.count || !name.data || !name.count ||
        parent.count > kUpdateArtifactMaximumPathCharacters || name.count > kUpdateStageFilenameCharacters ||
        parent.count + name.count + 1 > kUpdateArtifactMaximumPathCharacters)
        return result(UpdateStageError::invalid_input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(source), sizeof(*source)},
        {reinterpret_cast<const u8*>(&approved), sizeof(approved)},
        {reinterpret_cast<const u8*>(parent.data), parent.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(name.data), name.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return result(UpdateStageError::invalid_input);
    if (!source->file || source->file == INVALID_HANDLE_VALUE || output->file || !approved.version ||
        !approved.artifact_bytes || approved.artifact_bytes > kUpdateArtifactMaximumBytes ||
        !approved.artifact_name_bytes || approved.artifact_name_bytes >= kUpdateArtifactNameCapacity ||
        approved.artifact_name[approved.artifact_name_bytes])
        return result(UpdateStageError::invalid_input);
    if (source->bytes != approved.artifact_bytes || source->version != approved.version ||
        !bytes_equal(source->sha256, approved.artifact_sha256, 32) ||
        !bytes_equal(source->manifest_sha256, approved.manifest_sha256, 32)) return result(UpdateStageError::identity);
    if (!parent_path(parent) || !stage_name(name)) return result(UpdateStageError::path);
    wchar_t path[kUpdateArtifactMaximumPathCharacters + 1];
    for (u32 index = 0; index < parent.count; ++index) path[index] = parent[index];
    path[parent.count] = 0;
    CopyState state{};
    state.parent = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (state.parent == INVALID_HANDLE_VALUE) return state.finish(result(UpdateStageError::parent, GetLastError()));
    BY_HANDLE_FILE_INFORMATION parent_info{}, source_info{};
    if (GetFileType(state.parent) != FILE_TYPE_DISK || !GetFileInformationByHandle(state.parent, &parent_info))
        return state.finish(result(UpdateStageError::parent, GetLastError()));
    if (!(parent_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || parent_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return state.finish(result(UpdateStageError::parent));
    if (GetFileType(source->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(source->file, &source_info))
        return state.finish(result(UpdateStageError::source, GetLastError()));
    if (source_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return state.finish(result(UpdateStageError::source));
    LARGE_INTEGER size{}, zero{};
    if (!GetFileSizeEx(source->file, &size)) return state.finish(result(UpdateStageError::source, GetLastError()));
    if (size.QuadPart < 0 || static_cast<u64>(size.QuadPart) != approved.artifact_bytes)
        return state.finish(result(UpdateStageError::size));
    if (!SetFilePointerEx(source->file, zero, nullptr, FILE_BEGIN)) return state.finish(result(UpdateStageError::read, GetLastError()));
    u32 path_count = parent.count;
    if (path[path_count - 1] != L'\\') path[path_count++] = L'\\';
    for (u32 index = 0; index < name.count; ++index) path[path_count++] = name[index];
    path[path_count] = 0;
    state.destination = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (state.destination == INVALID_HANDLE_VALUE) return state.finish(result(UpdateStageError::create, GetLastError()));
    BY_HANDLE_FILE_INFORMATION destination_info{};
    if (GetFileType(state.destination) != FILE_TYPE_DISK || !GetFileInformationByHandle(state.destination, &destination_info))
        return state.finish(result(UpdateStageError::create, GetLastError()));
    if (destination_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        destination_info.nNumberOfLinks != 1 || same_file(source_info, destination_info))
        return state.finish(result(UpdateStageError::create));
    NTSTATUS status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &state.hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) return state.finish(result(UpdateStageError::hash, 0, status));
    u8 buffer[kUpdateArtifactReadBufferBytes];
    u64 remaining = approved.artifact_bytes;
    while (remaining) {
        const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(source->file, buffer, wanted, &read, nullptr)) return state.finish(result(UpdateStageError::read, GetLastError()));
        if (!read || read > wanted) return state.finish(result(UpdateStageError::size));
        status = BCryptHashData(state.hash, buffer, read, 0);
        if (status < 0) return state.finish(result(UpdateStageError::hash, 0, status));
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(state.destination, buffer + offset, read - offset, &written, nullptr))
                return state.finish(result(UpdateStageError::write, GetLastError()));
            if (!written || written > read - offset) return state.finish(result(UpdateStageError::write));
            offset += written;
        }
        remaining -= read;
    }
    DWORD trailing = 0;
    if (!ReadFile(source->file, buffer, 1, &trailing, nullptr)) return state.finish(result(UpdateStageError::read, GetLastError()));
    if (trailing || !GetFileSizeEx(source->file, &size) || size.QuadPart < 0 ||
        static_cast<u64>(size.QuadPart) != approved.artifact_bytes) return state.finish(result(UpdateStageError::size));
    status = BCryptFinishHash(state.hash, state.digest, sizeof(state.digest), 0);
    if (status < 0) return state.finish(result(UpdateStageError::hash, 0, status));
    if (!bytes_equal(state.digest, approved.artifact_sha256, 32)) return state.finish(result(UpdateStageError::digest));
    status = BCryptDestroyHash(state.hash);
    if (status < 0) return state.finish(result(UpdateStageError::hash, 0, status));
    state.hash = nullptr;
    if (!GetFileSizeEx(state.destination, &size) || size.QuadPart < 0 ||
        static_cast<u64>(size.QuadPart) != approved.artifact_bytes) return state.finish(result(UpdateStageError::size));
    if (!FlushFileBuffers(state.destination)) return state.finish(result(UpdateStageError::flush, GetLastError()));
    if (!SetFilePointerEx(state.destination, zero, nullptr, FILE_BEGIN)) return state.finish(result(UpdateStageError::retain, GetLastError()));
    if (!DuplicateHandle(GetCurrentProcess(), state.destination, GetCurrentProcess(), &state.retained,
        FILE_GENERIC_READ | DELETE, FALSE, 0)) return state.finish(result(UpdateStageError::retain, GetLastError()));
    if (!CloseHandle(state.parent)) return state.finish(result(UpdateStageError::parent, GetLastError()));
    state.parent = INVALID_HANDLE_VALUE;
    if (!CloseHandle(state.destination)) return state.finish(result(UpdateStageError::retain, GetLastError()));
    state.destination = INVALID_HANDLE_VALUE;
    UpdateStage candidate{};
    candidate.file = state.retained;
    candidate.bytes = approved.artifact_bytes;
    candidate.version = approved.version;
    copy_bytes(candidate.sha256, state.digest, 32);
    copy_bytes(candidate.manifest_sha256, approved.manifest_sha256, 32);
    copy_bytes(candidate.artifact_name, approved.artifact_name, approved.artifact_name_bytes);
    candidate.artifact_name_bytes = approved.artifact_name_bytes;
    *output = candidate;
    state.retained = nullptr;
    return state.finish(result(UpdateStageError::none));
}

UpdateStageResult update_stage_release(UpdateStage* stage, bool discard) noexcept {
    if (!stage || stage->file == INVALID_HANDLE_VALUE) return result(UpdateStageError::invalid_input);
    if (stage->file && discard) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(stage->file, FileDispositionInfo, &disposition, sizeof(disposition)))
            return result(UpdateStageError::release, GetLastError());
    }
    if (stage->file && !CloseHandle(stage->file)) return result(UpdateStageError::release, GetLastError());
    SecureZeroMemory(stage, sizeof(*stage));
    return result(UpdateStageError::none);
}

UpdateStageResult update_stage_cleanup_release(UpdateStageResult* failed_copy) noexcept {
    if (!failed_copy) return result(UpdateStageError::invalid_input);
    DWORD native = 0;
    i32 crypto = 0;
    if (failed_copy->cleanup_hash) {
        crypto = BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(failed_copy->cleanup_hash));
        if (crypto >= 0) failed_copy->cleanup_hash = nullptr;
    }
    if (failed_copy->cleanup_file) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(failed_copy->cleanup_file, FileDispositionInfo, &disposition, sizeof(disposition)))
            native = GetLastError();
        else if (!CloseHandle(failed_copy->cleanup_file)) native = GetLastError();
        else failed_copy->cleanup_file = nullptr;
    }
    if (failed_copy->cleanup_duplicate) {
        if (!CloseHandle(failed_copy->cleanup_duplicate)) { if (!native) native = GetLastError(); }
        else failed_copy->cleanup_duplicate = nullptr;
    }
    if (failed_copy->cleanup_parent) {
        if (!CloseHandle(failed_copy->cleanup_parent)) { if (!native) native = GetLastError(); }
        else failed_copy->cleanup_parent = nullptr;
    }
    failed_copy->cleanup_error = native;
    if (crypto < 0) {
        failed_copy->crypto_status = crypto;
        return result(UpdateStageError::hash, native, crypto);
    }
    return result(native ? UpdateStageError::release : UpdateStageError::none, native);
}
} // namespace maicord

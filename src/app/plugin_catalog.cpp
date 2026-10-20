#include "maicord/app/plugin_catalog.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/core/win32_file_info.hpp"

namespace maicord {
static_assert(kNativePluginFileRead == GENERIC_READ && kNativePluginFileShareRead == FILE_SHARE_READ &&
              kNativePluginFileOpenExisting == OPEN_EXISTING &&
              kNativePluginFileSequentialScan == FILE_FLAG_SEQUENTIAL_SCAN);
namespace {
NativePluginCatalogResult result(NativePluginCatalogError error, DWORD native = 0, i32 crypto = 0) noexcept {
    return {error, native, crypto};
}
bool directory_valid(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 3 || path.count >= kNativePluginCatalogPathCapacity ||
        ascii_lower(path[0]) < L'a' || ascii_lower(path[0]) > L'z' || path[1] != L':' || path[2] != L'\\') return false;
    u32 start = 3, ancestors = 1;
    for (u32 i = 3; i <= path.count; ++i) if (i == path.count || path[i] == L'\\') {
        if (i == 3 && path.count == 3) return true;
        if (!native_plugin_path_component_valid({path.data + start, i - start}) || ++ancestors > kNativePluginPathMaximumAncestors) return false;
        start = i + 1;
    }
    return true;
}
// Every directory component is opened without following its own reparse point,
// then pinned while descendants are opened. Root-to-leaf pinning prevents a
// checked ancestor from being renamed/replaced between hash and DLL entry.
NativePluginCatalogResult pin_directory(ConstSlice<wchar_t> directory, NativePluginVerifiedFile* guard,u32 create_tail=0) noexcept {
    if (!directory_valid(directory)) return result(NativePluginCatalogError::path);
    u32 total=1;
    if(directory.count>3)for(u32 i=3;i<=directory.count;++i)if(i==directory.count||directory[i]==L'\\')++total;
    if(create_tail>2 || create_tail>=total)return result(NativePluginCatalogError::path);
    for (u32 i = 0; i < directory.count; ++i) guard->path[i] = directory[i];
    guard->path[directory.count] = 0;
    guard->path_characters = directory.count;
    for (u32 end = 3; end <= directory.count; ++end) {
        if (end != 3 && end != directory.count && directory[end] != L'\\') continue;
        if (end == 3 && directory.count == 3 && guard->ancestor_count) continue;
        const wchar_t saved = guard->path[end];
        guard->path[end] = 0;
        HANDLE handle = CreateFileW(guard->path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(handle==INVALID_HANDLE_VALUE && create_tail && guard->ancestor_count>=total-create_tail &&
            (GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)){
            if(CreateDirectoryW(guard->path,nullptr)||GetLastError()==ERROR_ALREADY_EXISTS)
                handle=CreateFileW(guard->path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        }
        guard->path[end] = saved;
        if (handle == INVALID_HANDLE_VALUE) return result(NativePluginCatalogError::directory, GetLastError());
        guard->ancestors[guard->ancestor_count++] = handle;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) return result(NativePluginCatalogError::directory, GetLastError());
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return result(NativePluginCatalogError::path);
    }
    return result(NativePluginCatalogError::none);
}
struct Guard {
    NativePluginVerifiedFile held{};
    ~Guard() noexcept { native_plugin_verified_file_release(&held); }
    NativePluginCatalogResult finish(NativePluginVerifiedFile* output, NativePluginCatalogResult failure) noexcept {
        const auto released = native_plugin_verified_file_release(&held);
        if (released.ok()) return failure;
        // A failed close is ownership, not an empty error result. The caller
        // allocated its stable gate before this operation and can retry release.
        *output = held; held = {};
        return released;
    }
};
bool append(NativePluginVerifiedFile* guard, ConstSlice<wchar_t> basename) noexcept {
    const u32 separator = guard->path_characters == 3 ? 0 : 1;
    if (!native_plugin_path_component_valid(basename) || guard->path_characters + separator + basename.count >= kNativePluginCatalogPathCapacity) return false;
    if (separator) guard->path[guard->path_characters++] = L'\\';
    for (u32 i = 0; i < basename.count; ++i) guard->path[guard->path_characters++] = basename[i];
    guard->path[guard->path_characters] = 0;
    return true;
}
NativePluginCatalogResult open_file(NativePluginVerifiedFile* guard,DWORD access=GENERIC_READ,
    DWORD sharing=FILE_SHARE_READ,DWORD creation=OPEN_EXISTING,DWORD flags=FILE_FLAG_SEQUENTIAL_SCAN) noexcept {
    HANDLE file = CreateFileW(guard->path, access|FILE_READ_ATTRIBUTES, sharing, nullptr, creation,
        flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result(NativePluginCatalogError::open, GetLastError());
    guard->file = file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info))
        return result(NativePluginCatalogError::file, GetLastError());
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return result(NativePluginCatalogError::file);
    // Reject DOS drive aliases and any normalized target that escaped the exact
    // pinned drive-rooted path. Extended namespaces remain an internal Win32
    // return format, never a manifest/catalog input or loader fallback.
    wchar_t final_path[kNativePluginCatalogPathCapacity + 4];
    const DWORD final_count = GetFinalPathNameByHandleW(file, final_path,
        kNativePluginCatalogPathCapacity + 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!final_count) return result(NativePluginCatalogError::file, GetLastError());
    if (final_count != guard->path_characters + 4 || final_path[0] != L'\\' || final_path[1] != L'\\' ||
        final_path[2] != L'?' || final_path[3] != L'\\') return result(NativePluginCatalogError::path);
    for (u32 i = 0; i < guard->path_characters; ++i)
        if (ascii_lower(final_path[i + 4]) != ascii_lower(guard->path[i])) return result(NativePluginCatalogError::path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return result(NativePluginCatalogError::file, GetLastError());
    if (size.QuadPart < 0 || (!size.QuadPart && creation!=CREATE_NEW)) return result(NativePluginCatalogError::size);
    guard->bytes = static_cast<u64>(size.QuadPart);
    return result(NativePluginCatalogError::none);
}
bool suffix(const wchar_t* name, u32 count) noexcept {
    constexpr u32 length = sizeof(kNativePluginManifestSuffix) / sizeof(wchar_t) - 1;
    if (count <= length) return false;
    for (u32 i = 0; i < length; ++i) if (ascii_lower(name[count - length + i]) != kNativePluginManifestSuffix[i]) return false;
    return true;
}
int filename_compare(const NativePluginCatalogEntry& a, const NativePluginCatalogEntry& b) noexcept {
    const u32 count = minimum(a.filename_characters, b.filename_characters);
    for (u32 i = 0; i < count; ++i) if (ascii_lower(a.filename[i]) != ascii_lower(b.filename[i])) return ascii_lower(a.filename[i]) < ascii_lower(b.filename[i]) ? -1 : 1;
    if (a.filename_characters != b.filename_characters) return a.filename_characters < b.filename_characters ? -1 : 1;
    for (u32 i = 0; i < count; ++i) if (a.filename[i] != b.filename[i]) return a.filename[i] < b.filename[i] ? -1 : 1;
    return 0;
}
bool same_dll(const NativePluginManifest& a, const NativePluginManifest& b) noexcept {
    if (a.dll_bytes != b.dll_bytes) return false;
    for (u32 i = 0; i < a.dll_bytes; ++i) if (ascii_lower(a.dll[i]) != ascii_lower(b.dll[i])) return false;
    return true;
}
NativePluginCatalogResult read_manifest(const NativePluginCatalog& catalog, NativePluginCatalogEntry* entry,
    NativePluginVerifiedFile* scan_owner) noexcept {
    Guard guard;
    // The scan owner already pins every canonical ancestor. Only this direct
    // manifest's file handle is acquired, read and released here.
    for (u32 i = 0; i < catalog.directory_characters; ++i) guard.held.path[i] = catalog.directory[i];
    guard.held.path_characters = catalog.directory_characters;
    auto opened = append(&guard.held, {entry->filename, entry->filename_characters}) ?
        result(NativePluginCatalogError::none) : result(NativePluginCatalogError::path);
    if (opened.ok()) opened = open_file(&guard.held);
    if (opened.ok() && guard.held.bytes > kNativePluginManifestMaximumBytes) opened = result(NativePluginCatalogError::size);
    char bytes[kNativePluginManifestMaximumBytes];
    DWORD read = 0, trailing = 0;
    char extra = 0;
    if (opened.ok() && (!ReadFile(guard.held.file, bytes, static_cast<DWORD>(guard.held.bytes), &read, nullptr) ||
        read != guard.held.bytes || !ReadFile(guard.held.file, &extra, 1, &trailing, nullptr)))
        opened = result(NativePluginCatalogError::read, GetLastError());
    if (opened.ok() && trailing) opened = result(NativePluginCatalogError::size);
    if (!opened.ok()) { entry->status = opened.error; entry->win32_error = opened.win32_error; }
    else {
        const auto parsed = native_plugin_manifest_parse({bytes, read}, &entry->manifest);
        if (!parsed.ok()) { entry->status = NativePluginCatalogError::manifest; entry->manifest_error = parsed.error; }
    }
    const auto released = native_plugin_verified_file_release(&guard.held);
    if (!released.ok()) {
        scan_owner->file = guard.held.file; guard.held.file = nullptr;
        entry->status = released.error; entry->win32_error = released.win32_error;
        return released;
    }
    return result(NativePluginCatalogError::none);
}
bool catalog_valid(const NativePluginCatalog& catalog) noexcept {
    return catalog.entries.data && catalog.entries.count && catalog.entries.count <= kNativePluginCatalogMaximumEntries &&
        directory_valid({catalog.directory, catalog.directory_characters}) && catalog.count <= catalog.entries.count &&
        byte_ranges_disjoint({reinterpret_cast<const u8*>(&catalog), sizeof(catalog)},
            {reinterpret_cast<const u8*>(catalog.entries.data), catalog.entries.count * static_cast<u32>(sizeof(*catalog.entries.data))});
}
} // namespace

NativePluginCatalogResult native_plugin_verified_file_release(NativePluginVerifiedFile* file) noexcept {
    if (!file || file->ancestor_count > kNativePluginPathMaximumAncestors) return result(NativePluginCatalogError::invalid_input);
    if (file->hash) {
        const NTSTATUS crypto = BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(file->hash));
        if (crypto < 0) return result(NativePluginCatalogError::release, 0, crypto);
        file->hash = nullptr;
    }
    if (file->scan) {
        if (!FindClose(file->scan)) return result(NativePluginCatalogError::release, GetLastError());
        file->scan = nullptr;
    }
    if (file->algorithm) {
        const NTSTATUS crypto = BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(file->algorithm), 0);
        if (crypto < 0) return result(NativePluginCatalogError::release, 0, crypto);
        file->algorithm = nullptr;
    }
    DWORD error = 0;
    if (file->file) { if (CloseHandle(file->file)) file->file = nullptr; else error = GetLastError(); }
    while (file->ancestor_count) {
        HANDLE& handle = file->ancestors[file->ancestor_count - 1];
        if (!CloseHandle(handle)) { if (!error) error = GetLastError(); break; }
        handle = nullptr; --file->ancestor_count;
    }
    if (error) return result(NativePluginCatalogError::release, error);
    *file = {};
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_catalog_initialize(NativePluginCatalog* catalog,
    ConstSlice<wchar_t> directory, Slice<NativePluginCatalogEntry> storage) noexcept {
    if (!catalog || catalog->cleanup || !storage.data || !storage.count || storage.count > kNativePluginCatalogMaximumEntries ||
        !directory_valid(directory)) return result(NativePluginCatalogError::invalid_input);
    const ConstSlice<u8> object{reinterpret_cast<const u8*>(catalog), sizeof(*catalog)};
    const ConstSlice<u8> entries{reinterpret_cast<const u8*>(storage.data), storage.count * static_cast<u32>(sizeof(*storage.data))};
    const ConstSlice<u8> path{reinterpret_cast<const u8*>(directory.data), directory.count * 2};
    if (!byte_ranges_disjoint(object, entries) || !byte_ranges_disjoint(object, path) || !byte_ranges_disjoint(entries, path)) return result(NativePluginCatalogError::invalid_input);
    *catalog = {};
    catalog->entries = storage;
    catalog->directory_characters = directory.count;
    for (u32 i = 0; i < directory.count; ++i) catalog->directory[i] = directory[i];
    for (u32 i = 0; i < storage.count; ++i) storage[i] = {};
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_catalog_release(NativePluginCatalog* catalog) noexcept {
    if (!catalog) return result(NativePluginCatalogError::invalid_input);
    if (!catalog->cleanup) return result(NativePluginCatalogError::none);
    const auto released = native_plugin_verified_file_release(catalog->cleanup);
    if (!released.ok()) return released;
    if (!VirtualFree(catalog->cleanup, 0, MEM_RELEASE)) return result(NativePluginCatalogError::release, GetLastError());
    catalog->cleanup = nullptr;
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_catalog_refresh(NativePluginCatalog* catalog) noexcept {
    if (!catalog || !catalog_valid(*catalog)) return result(NativePluginCatalogError::invalid_input);
    catalog->count = 0;
    auto outcome = native_plugin_catalog_release(catalog);
    if (!outcome.ok()) return outcome;
    catalog->cleanup = static_cast<NativePluginVerifiedFile*>(VirtualAlloc(nullptr,
        sizeof(NativePluginVerifiedFile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!catalog->cleanup) return result(NativePluginCatalogError::capacity, GetLastError());
    auto& owner = *catalog->cleanup;
    outcome = pin_directory({catalog->directory, catalog->directory_characters}, &owner);
    const auto finish = [catalog](NativePluginCatalogResult pending) noexcept {
        const auto released = native_plugin_catalog_release(catalog);
        return released.ok() ? pending : released;
    };
    if (!outcome.ok()) return finish(outcome);
    // Wildcard is appended only after canonical directory validation/pinning.
    const u32 base = owner.path_characters;
    if (base + 3 >= kNativePluginCatalogPathCapacity) return finish(result(NativePluginCatalogError::path));
    u32 used = base;
    if (used != 3) owner.path[used++] = L'\\';
    owner.path[used++] = L'*'; owner.path[used] = 0;
    WIN32_FIND_DATAW data{};
    HANDLE scan = FindFirstFileExW(owner.path, FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (scan == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return finish(error == ERROR_FILE_NOT_FOUND ? result(NativePluginCatalogError::none) : result(NativePluginCatalogError::scan, error));
    }
    owner.scan = scan;
    u32 visited = 0;
    do {
        if ((data.cFileName[0] == L'.' && !data.cFileName[1]) ||
            (data.cFileName[0] == L'.' && data.cFileName[1] == L'.' && !data.cFileName[2])) continue;
        if (++visited > kNativePluginCatalogMaximumDirectoryEntries) { outcome = result(NativePluginCatalogError::capacity); break; }
        u32 length = 0;
        while (length < MAX_PATH && data.cFileName[length]) ++length;
        if (!suffix(data.cFileName, length)) continue;
        if (catalog->count == catalog->entries.count || length >= kNativePluginCatalogFilenameCapacity) {
            outcome = result(NativePluginCatalogError::capacity); break;
        }
        auto& entry = catalog->entries[catalog->count++]; entry = {};
        entry.filename_characters = length;
        for (u32 i = 0; i < length; ++i) entry.filename[i] = data.cFileName[i];
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) entry.status = NativePluginCatalogError::file;
        else {
            outcome = read_manifest(*catalog, &entry, &owner);
            if (!outcome.ok()) break;
        }
    } while (FindNextFileW(scan, &data));
    const DWORD scan_error = GetLastError();
    if (outcome.ok() && scan_error != ERROR_NO_MORE_FILES) outcome = result(NativePluginCatalogError::scan, scan_error);
    // A manifest close failure already transferred ownership into the stable
    // scan owner; do not implicitly retry that failure before the caller sees it.
    if (outcome.error != NativePluginCatalogError::release) outcome = finish(outcome);
    if (!outcome.ok()) { catalog->count = 0; return outcome; }
    for (u32 i = 1; i < catalog->count; ++i) {
        auto item = catalog->entries[i]; u32 j = i;
        while (j && filename_compare(item, catalog->entries[j - 1]) < 0) { catalog->entries[j] = catalog->entries[j - 1]; --j; }
        catalog->entries[j] = item;
    }
    // Compare every parsed entry including already-conflicted ones so a chain
    // cannot leave a later alias or owner accidentally enableable.
    for (u32 i = 0; i < catalog->count; ++i) for (u32 j = 0; j < i; ++j) {
        auto& a = catalog->entries[i]; auto& b = catalog->entries[j];
        if (!a.manifest.owner_id || !b.manifest.owner_id) continue;
        if (a.manifest.owner_id == b.manifest.owner_id) a.status = b.status = NativePluginCatalogError::duplicate_owner;
        else if (same_dll(a.manifest, b.manifest)) {
            if (a.status != NativePluginCatalogError::duplicate_owner) a.status = NativePluginCatalogError::duplicate_dll;
            if (b.status != NativePluginCatalogError::duplicate_owner) b.status = NativePluginCatalogError::duplicate_dll;
        }
    }
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_declared_file_verify(ConstSlice<wchar_t> directory,
    const NativePluginManifest& manifest, NativePluginVerifiedFile* output) noexcept {
    if (!output || output->hash || output->algorithm || output->scan || output->file || output->ancestor_count || !native_plugin_manifest_valid(manifest) || !directory_valid(directory))
        return result(NativePluginCatalogError::invalid_input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(directory.data), directory.count * 2},
        {reinterpret_cast<const u8*>(&manifest), sizeof(manifest)}, {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    for (u32 i = 0; i < 3; ++i) for (u32 j = 0; j < i; ++j) if (!byte_ranges_disjoint(ranges[i], ranges[j])) return result(NativePluginCatalogError::invalid_input);
    Guard guard;
    auto outcome = pin_directory(directory, &guard.held);
    wchar_t filename[kNativePluginDllNameCapacity];
    for (u32 i = 0; i < manifest.dll_bytes; ++i) filename[i] = static_cast<wchar_t>(manifest.dll[i]);
    if (outcome.ok() && !append(&guard.held, {filename, manifest.dll_bytes})) outcome = result(NativePluginCatalogError::path);
    if (outcome.ok()) outcome = open_file(&guard.held);
    if (!outcome.ok()) return guard.finish(output, outcome);
    if (guard.held.bytes > kNativePluginDllMaximumBytes) return guard.finish(output, result(NativePluginCatalogError::size));
    outcome = native_plugin_verified_file_hash(&guard.held, {manifest.sha256, 32});
    if (outcome.error == NativePluginCatalogError::release) { *output = guard.held; guard.held = {}; return outcome; }
    if (!outcome.ok()) return guard.finish(output, outcome);
    *output = guard.held; guard.held = {};
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_verified_file_hash(NativePluginVerifiedFile* file, ConstSlice<u8> expected) noexcept {
    if (!file || !file->file || file->hash || file->algorithm || !expected.data || expected.count != 32 ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(file),sizeof(*file)},expected)) return result(NativePluginCatalogError::invalid_input);
    if (!file->bytes || file->bytes > kNativePluginDllMaximumBytes) return result(NativePluginCatalogError::size);
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file->file,zero,nullptr,FILE_BEGIN)) return result(NativePluginCatalogError::read,GetLastError());
    auto outcome = result(NativePluginCatalogError::none);
    u8 buffer[8192], digest[32];
    DWORD object_bytes = 0, returned = 0;
    BCRYPT_ALG_HANDLE algorithm{};
    NTSTATUS crypto = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (crypto < 0) return result(NativePluginCatalogError::hash, 0, crypto);
    file->algorithm = algorithm;
    crypto = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned, 0);
    if (crypto < 0 || returned != sizeof(object_bytes) || !object_bytes || object_bytes > 4096)
        return result(NativePluginCatalogError::hash, 0, crypto);
    BCRYPT_HASH_HANDLE hash{};
    // CNG owns the bounded object so a failed destruction never leaves its live
    // handle pointing into expired caller stack storage (supported since Win7).
    crypto = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (crypto < 0) return result(NativePluginCatalogError::hash, 0, crypto);
    file->hash = hash;
    u64 remaining = file->bytes;
    while (remaining && outcome.ok()) {
        const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(file->file, buffer, wanted, &read, nullptr)) outcome = result(NativePluginCatalogError::read, GetLastError());
        else if (read != wanted) outcome = result(NativePluginCatalogError::size);
        else {
            crypto = BCryptHashData(hash, buffer, read, 0);
            if (crypto < 0) outcome = result(NativePluginCatalogError::hash, 0, crypto);
            remaining -= read;
        }
    }
    DWORD trailing = 0;
    if (outcome.ok() && !ReadFile(file->file, buffer, 1, &trailing, nullptr)) outcome = result(NativePluginCatalogError::read, GetLastError());
    if (outcome.ok() && trailing) outcome = result(NativePluginCatalogError::size);
    if (outcome.ok()) {
        crypto = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (crypto < 0) outcome = result(NativePluginCatalogError::hash, 0, crypto);
        else if (!bytes_equal(digest, expected.data, sizeof(digest))) outcome = result(NativePluginCatalogError::digest);
    }
    // Destroy the hash/provider before admitting any code, while preserving
    // the separately retained filesystem pins through load/query/start.
    crypto = BCryptDestroyHash(hash);
    if (crypto < 0) return result(NativePluginCatalogError::release, 0, crypto);
    file->hash = nullptr;
    crypto = BCryptCloseAlgorithmProvider(algorithm, 0);
    if (crypto < 0) return result(NativePluginCatalogError::release, 0, crypto);
    file->algorithm = nullptr;
    return outcome;
}
NativePluginCatalogResult native_plugin_directory_pin(ConstSlice<wchar_t> directory,
    NativePluginVerifiedFile* output,u32 create_tail) noexcept {
    if(!output || output->file || output->scan || output->hash || output->algorithm || output->ancestor_count ||
        !directory_valid(directory) || !byte_ranges_disjoint({reinterpret_cast<const u8*>(directory.data),directory.count*2},
            {reinterpret_cast<const u8*>(output),sizeof(*output)}))return result(NativePluginCatalogError::invalid_input);
    return pin_directory(directory,output,create_tail);
}
NativePluginCatalogResult native_plugin_pinned_file_open(const NativePluginVerifiedFile& directory,
    ConstSlice<wchar_t> basename,NativePluginVerifiedFile* output,DWORD access,DWORD sharing,DWORD creation,DWORD flags) noexcept {
    if(!output || output->file || output->scan || output->hash || output->algorithm || output->ancestor_count ||
        !directory.ancestor_count || directory.ancestor_count>kNativePluginPathMaximumAncestors || directory.file ||
        !directory_valid({directory.path,directory.path_characters}) || !basename.data ||
        basename.count>=kNativePluginCatalogFilenameCapacity || !native_plugin_path_component_valid(basename) ||
        (creation!=CREATE_NEW && creation!=OPEN_EXISTING) ||
        (creation==OPEN_EXISTING && (access&~(GENERIC_READ|DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE))) ||
        (sharing&FILE_SHARE_WRITE))return result(NativePluginCatalogError::invalid_input);
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(&directory),sizeof(directory)},
        {reinterpret_cast<const u8*>(basename.data),basename.count*2},{reinterpret_cast<const u8*>(output),sizeof(*output)}};
    for(u32 i=0;i<3;++i)for(u32 j=0;j<i;++j)if(!byte_ranges_disjoint(ranges[i],ranges[j]))return result(NativePluginCatalogError::invalid_input);
    for(u32 i=0;i<directory.path_characters;++i)output->path[i]=directory.path[i];
    output->path_characters=directory.path_characters;
    if(!append(output,basename))return result(NativePluginCatalogError::path);
    return open_file(output,access,sharing,creation,flags);
}
NativePluginCatalogResult native_plugin_pinned_manifest_read(const NativePluginVerifiedFile& directory,
    ConstSlice<wchar_t> basename,NativePluginManifest* manifest,NativePluginVerifiedFile* output,DWORD sharing) noexcept {
    if(!manifest || !output || !basename.data || !basename.count || basename.count>=kNativePluginCatalogFilenameCapacity ||
        (sharing!=FILE_SHARE_READ && sharing!=(FILE_SHARE_READ|FILE_SHARE_DELETE)) ||
        !suffix(basename.data,basename.count) ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(manifest),sizeof(*manifest)},
            {reinterpret_cast<const u8*>(output),sizeof(*output)}) ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(manifest),sizeof(*manifest)},
            {reinterpret_cast<const u8*>(&directory),sizeof(directory)}) ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(manifest),sizeof(*manifest)},
            {reinterpret_cast<const u8*>(basename.data),basename.count*2}))return result(NativePluginCatalogError::invalid_input);
    auto checked=native_plugin_pinned_file_open(directory,basename,output,GENERIC_READ,sharing);
    if(!checked.ok())return checked;
    if(output->bytes>kNativePluginManifestMaximumBytes)return result(NativePluginCatalogError::size);
    char bytes[kNativePluginManifestMaximumBytes],extra{};DWORD read{},trailing{};
    if(!ReadFile(output->file,bytes,static_cast<DWORD>(output->bytes),&read,nullptr)||read!=output->bytes ||
        !ReadFile(output->file,&extra,1,&trailing,nullptr))return result(NativePluginCatalogError::read,GetLastError());
    if(trailing)return result(NativePluginCatalogError::size);
    if(!native_plugin_manifest_parse({bytes,read},manifest).ok())return result(NativePluginCatalogError::manifest);
    return result(NativePluginCatalogError::none);
}
NativePluginCatalogResult native_plugin_pinned_dll_verify(const NativePluginVerifiedFile& directory,
    const NativePluginManifest& manifest,NativePluginVerifiedFile* output) noexcept {
    if(!output || !directory.ancestor_count || directory.ancestor_count>kNativePluginPathMaximumAncestors || directory.file ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(&directory),sizeof(directory)},
            {reinterpret_cast<const u8*>(output),sizeof(*output)}))return result(NativePluginCatalogError::invalid_input);
    // Reuse the accepted hash walker and its full retryable ownership gate.
    // This verifier also retains independent ancestor pins through the copy.
    return native_plugin_declared_file_verify({directory.path,directory.path_characters},manifest,output);
}
} // namespace maicord

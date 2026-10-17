#include "maicord/app/plugin_preferences.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include <stddef.h>

#include <Windows.h>

namespace maicord {
struct NativePluginPreferencesCleanup {
    HANDLE file;
    HANDLE ancestors[64];
    u32 ancestor_count;
    bool temporary;
    wchar_t path[1024];
    u32 path_characters;
    u8 record[kNativePluginPreferencesRecordBytes];
};
static_assert(sizeof(NativePluginPreferencesCleanup) <= 4096);
namespace {
using Error = NativePluginPreferencesError;
constexpr u8 kMagic[8]{'M','A','I','P','L','U','G','S'};
NativePluginPreferencesResult fail(Error error, DWORD native = 0) noexcept { return {error, native, false}; }
bool disjoint(ConstSlice<u8> a, ConstSlice<u8> b) noexcept {
    return byte_range_valid(a) && byte_range_valid(b) && !byte_ranges_overlap(a,b);
}
bool same_leaf(const NativePluginPreference& a, const NativePluginPreference& b) noexcept {
    return a.dll_bytes == b.dll_bytes && bytes_equal(a.dll, b.dll, a.dll_bytes);
}
bool leaf_valid(const char* dll, u32 count) noexcept {
    if (count < 5 || count >= kNativePluginDllNameCapacity || dll[0] == '.' ||
        dll[count - 4] != '.' || dll[count - 3] != 'd' || dll[count - 2] != 'l' || dll[count - 1] != 'l') return false;
    u32 stem = 0;
    for (u32 i = 0; i < count; ++i) {
        const char c = dll[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') ||
            (c == '.' && i && dll[i - 1] == '.')) return false;
        if (!stem && c == '.') stem = i;
    }
    if (stem == 3 && (bytes_equal(dll,"con",3) || bytes_equal(dll,"prn",3) ||
        bytes_equal(dll,"aux",3) || bytes_equal(dll,"nul",3))) return false;
    if (stem == 4 && dll[3] >= '1' && dll[3] <= '9' &&
        (bytes_equal(dll,"com",3) || bytes_equal(dll,"lpt",3))) return false;
    return true;
}
bool preference_valid(const NativePluginPreference& entry) noexcept {
    if (!entry.owner_id || entry.abi_version != 5 || !leaf_valid(entry.dll, entry.dll_bytes) ||
        (entry.flags & ~native_plugin_preference_known_flags) || (entry.permissions & ~kNativePluginPermissionsAll)) return false;
    for (u32 i = entry.dll_bytes; i < sizeof(entry.dll); ++i) if (entry.dll[i]) return false;
    return true;
}
NativePluginPreference identity(const NativePluginManifest& manifest) noexcept {
    NativePluginPreference key{};
    key.owner_id = manifest.owner_id; key.abi_version = manifest.abi_version; key.dll_bytes = manifest.dll_bytes;
    copy_bytes(key.sha256, manifest.sha256, 32);
    for (u32 i = 0; i < manifest.dll_bytes; ++i) key.dll[i] = ascii_lower(manifest.dll[i]);
    return key;
}
bool same_identity(const NativePluginPreference& a, const NativePluginPreference& b) noexcept {
    return bytes_equal(&a, &b, kNativePluginPreferenceIdentityBytes);
}
u64 checksum(ConstSlice<u8> record) noexcept {
    return hash64_bytes(hash64_bytes(kHash64Seed,record.data,24),record.data + 32,record.count - 32);
}
bool path_valid(const wchar_t* path, u32* length, u32* parent) noexcept {
    *length = wide_length(path,1024);
    if (*length < 4 || *length + 20 >= 1024 || ascii_lower(path[0]) < L'a' || ascii_lower(path[0]) > L'z' ||
        path[1] != L':' || path[2] != L'\\') return false;
    u32 start=3, ancestors=1; *parent=3;
    for (u32 i=3;i<=*length;++i) if (i==*length || path[i]==L'\\') {
        if (!native_plugin_path_component_valid({path+start,i-start})) return false;
        if (i!=*length) { *parent=i; if (++ancestors>64) return false; }
        start=i+1;
    }
    return true;
}
bool final_path_equal(HANDLE file, const wchar_t* path, u32 count) noexcept {
    wchar_t final_path[1028];
    const DWORD got=GetFinalPathNameByHandleW(file,final_path,1028,FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (got!=count+4 || final_path[0]!=L'\\' || final_path[1]!=L'\\' || final_path[2]!=L'?' || final_path[3]!=L'\\') return false;
    for(u32 i=0;i<count;++i) if(ascii_lower(final_path[i+4])!=ascii_lower(path[i])) return false;
    return true;
}
NativePluginPreferencesResult pin(NativePluginPreferencesCleanup& held,u32 parent,bool create) noexcept {
    for(u32 end=3;end<=parent;++end) {
        if(end!=3 && end!=parent && held.path[end]!=L'\\') continue;
        if(end==3 && parent==3 && held.ancestor_count) continue;
        const wchar_t saved=held.path[end]; held.path[end]=0;
        HANDLE dir=CreateFileW(held.path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        DWORD error=dir==INVALID_HANDLE_VALUE?GetLastError():0;
        // Default saves create just the final user-owned MaiCord directory.
        if(dir==INVALID_HANDLE_VALUE && create && end==parent &&
            (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)) {
            if(CreateDirectoryW(held.path,nullptr) || GetLastError()==ERROR_ALREADY_EXISTS)
                dir=CreateFileW(held.path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            error=dir==INVALID_HANDLE_VALUE?GetLastError():0;
        }
        if(dir==INVALID_HANDLE_VALUE) {held.path[end]=saved; return fail(Error::directory,error);}
        held.ancestors[held.ancestor_count++]=dir;
        BY_HANDLE_FILE_INFORMATION info{};
        const bool okay=GetFileInformationByHandle(dir,&info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && final_path_equal(dir,held.path,end);
        held.path[end]=saved;
        if(!okay) return fail(Error::path);
    }
    return {};
}
NativePluginPreferencesResult begin(NativePluginPreferencesIo* io,const wchar_t* path,u32* parent) noexcept {
    if(!io) return fail(Error::invalid_input);
    if(io->cleanup) return fail(Error::busy);
    u32 count=0;
    if(!path_valid(path,&count,parent)) return fail(Error::path);
    if(!disjoint({reinterpret_cast<const u8*>(io),sizeof(*io)},
        {reinterpret_cast<const u8*>(path),(count+1)*2})) return fail(Error::invalid_input);
    io->cleanup=static_cast<NativePluginPreferencesCleanup*>(VirtualAlloc(nullptr,sizeof(NativePluginPreferencesCleanup),
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!io->cleanup) return fail(Error::allocation,GetLastError());
    copy_bytes(io->cleanup->path,path,(count+1)*2); io->cleanup->path_characters=count;
    return {};
}
NativePluginPreferencesResult finish(NativePluginPreferencesIo* io,NativePluginPreferencesResult result) noexcept {
    auto released=native_plugin_preferences_release(io);
    released.committed=result.committed;
    return released.ok()?result:released;
}
NativePluginPreferencesResult open_read(NativePluginPreferencesCleanup& held) noexcept {
    held.file=CreateFileW(held.path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(held.file==INVALID_HANDLE_VALUE) {
        held.file=nullptr; const DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND ? NativePluginPreferencesResult{Error::none,error,true}:fail(Error::open,error);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(held.file)!=FILE_TYPE_DISK || !GetFileInformationByHandle(held.file,&info) ||
        info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return fail(Error::file);
    return final_path_equal(held.file,held.path,held.path_characters)?NativePluginPreferencesResult{}:fail(Error::path);
}
NativePluginPreferencesResult user_path(wchar_t* path) noexcept {
    // Leaves four spare characters below the 1024-character path limit.
    const DataPathResult built=data_path(path,1020,L"plugins.bin");
    return built.ok()?NativePluginPreferencesResult{}:fail(Error::path,built.native_error);
}
NativePluginPreferencesResult save_file(NativePluginPreferencesIo* io,const wchar_t* path,
    const NativePluginPreferences& preferences,bool create) noexcept {
    if(!io || !native_plugin_preferences_valid(preferences) ||
        !disjoint({reinterpret_cast<const u8*>(io),sizeof(*io)},
            {reinterpret_cast<const u8*>(&preferences),sizeof(preferences)})) return fail(Error::invalid_input);
    const u32 path_count=wide_length(path,1024);
    if(!path || path_count>=1024 || !disjoint({reinterpret_cast<const u8*>(path),(path_count+1)*2},
        {reinterpret_cast<const u8*>(&preferences),sizeof(preferences)})) return fail(Error::invalid_input);
    u32 parent=0; auto outcome=begin(io,path,&parent); if(!outcome.ok()) return outcome;
    auto& held=*io->cleanup;
    outcome=pin(held,parent,create);
    if(!outcome.ok()) return finish(io,outcome);
    // Validate an existing destination without following a link. No writes.
    outcome=open_read(held);
    if(!outcome.ok()) return finish(io,outcome);
    if(held.file) {
        if(!CloseHandle(held.file)) return fail(Error::release,GetLastError());
        held.file=nullptr;
    }
    outcome=native_plugin_preferences_encode(preferences,{held.record,sizeof(held.record)});
    if(!outcome.ok()) return finish(io,outcome);
    const u32 base=held.path_characters;
    held.path[base]=L'.';held.path[base+1]=L'n';held.path[base+2]=L'e';held.path[base+3]=L'w';
    DWORD create_error=ERROR_FILE_EXISTS;
    const u32 temporary_count=base+20;
    // UI-thread-only sequence, mixed with monotonic uptime and process ID.
    // Uniqueness is confirmed by CREATE_NEW, never assumed from the suffix.
    // Each transaction gets fresh candidates rather than a permanent slot set.
    static u64 sequence=0;
    u64 suffix=hash64_mix(hash64_mix(GetTickCount64(),GetCurrentProcessId()),++sequence);
    constexpr wchar_t hexadecimal[]=L"0123456789abcdef";
    for(u32 attempt=0;attempt<16;++attempt) {
        for(u32 i=0;i<16;++i) held.path[base+4+i]=hexadecimal[(suffix>>((15-i)*4))&15];
        held.path[temporary_count]=0;
        held.file=CreateFileW(held.path,GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(held.file!=INVALID_HANDLE_VALUE) break;
        create_error=GetLastError();held.file=nullptr;
        if(create_error!=ERROR_FILE_EXISTS && create_error!=ERROR_ALREADY_EXISTS) return finish(io,fail(Error::open,create_error));
        ++suffix;
    }
    if(!held.file) return finish(io,fail(Error::open,create_error));
    held.temporary=true;
    if(!final_path_equal(held.file,held.path,temporary_count)) return finish(io,fail(Error::path));
    DWORD written=0;
    if(!WriteFile(held.file,held.record,sizeof(held.record),&written,nullptr) || written!=sizeof(held.record))
        return finish(io,fail(Error::write,GetLastError()));
    if(!FlushFileBuffers(held.file)) return finish(io,fail(Error::flush,GetLastError()));
    // Rename the owned handle, never a source pathname that can be substituted.
    struct Rename { BOOL replace; HANDLE root; DWORD bytes; wchar_t name[1024]; } rename{};
    static_assert(offsetof(Rename,name)==offsetof(FILE_RENAME_INFO,FileName));
    rename.replace=TRUE;rename.bytes=base*2;
    for(u32 i=0;i<base;++i) rename.name[i]=held.path[i];
    if(!SetFileInformationByHandle(held.file,FileRenameInfo,&rename,
        static_cast<DWORD>(offsetof(Rename,name))+rename.bytes)) return finish(io,fail(Error::commit,GetLastError()));
    held.temporary=false;
    if(!FlushFileBuffers(held.file)) {auto result=fail(Error::flush,GetLastError());result.committed=true;return finish(io,result);}
    return finish(io,{Error::none,0,false,true});
}
} // namespace

bool native_plugin_preferences_valid(const NativePluginPreferences& preferences) noexcept {
    if(preferences.count>kNativePluginPreferencesMaximumEntries) return false;
    for(u32 i=0;i<preferences.count;++i) {
        if(!preference_valid(preferences.entries[i])) return false;
        for(u32 j=0;j<i;++j) if(preferences.entries[i].owner_id==preferences.entries[j].owner_id ||
            same_leaf(preferences.entries[i],preferences.entries[j])) return false;
    }
    for(u32 i=preferences.count;i<kNativePluginPreferencesMaximumEntries;++i) {
        const auto* bytes=reinterpret_cast<const u8*>(&preferences.entries[i]);
        for(u32 j=0;j<sizeof(NativePluginPreference);++j) if(bytes[j]) return false;
    }
    return true;
}
const NativePluginPreference* native_plugin_preferences_find(const NativePluginPreferences& preferences,
    const NativePluginManifest& manifest) noexcept {
    if(!native_plugin_preferences_valid(preferences) || !native_plugin_manifest_valid(manifest)) return nullptr;
    const auto key=identity(manifest);
    for(u32 i=0;i<preferences.count;++i) if(same_identity(key,preferences.entries[i])) return &preferences.entries[i];
    return nullptr;
}
bool native_plugin_preferences_restorable(const NativePluginPreferences& preferences,const NativePluginManifest& manifest) noexcept {
    const auto* entry=native_plugin_preferences_find(preferences,manifest);
    return entry && !entry->flags && !(manifest.permissions & ~static_cast<u64>(entry->permissions));
}
bool native_plugin_preferences_set_flags(NativePluginPreferences* preferences,u64 owner,u32 flags) noexcept {
    if(!preferences || !owner || (flags & ~native_plugin_preference_known_flags) ||
        !native_plugin_preferences_valid(*preferences)) return false;
    for(u32 i=0;i<preferences->count;++i) if(preferences->entries[i].owner_id==owner) {
        preferences->entries[i].flags=flags;return true;
    }
    return false;
}
NativePluginPreferencesResult native_plugin_preferences_enable(NativePluginPreferences* preferences,const NativePluginManifest& manifest,u64 permissions) noexcept {
    if(!preferences || !native_plugin_preferences_valid(*preferences) || !native_plugin_manifest_valid(manifest) ||
        !disjoint({reinterpret_cast<const u8*>(preferences),sizeof(*preferences)},
            {reinterpret_cast<const u8*>(&manifest),sizeof(manifest)})) return fail(Error::invalid_input);
    if(permissions & ~manifest.permissions) return fail(Error::invalid_input);
    auto key=identity(manifest);key.permissions=static_cast<u32>(permissions);u32 index=preferences->count;
    for(u32 i=0;i<preferences->count;++i) {
        if(preferences->entries[i].owner_id==key.owner_id) index=i;
        else if(same_leaf(preferences->entries[i],key)) return fail(Error::invalid_input);
    }
    if(index==kNativePluginPreferencesMaximumEntries) return fail(Error::capacity);
    preferences->entries[index]=key;if(index==preferences->count) ++preferences->count;
    return {};
}
bool native_plugin_preferences_remove_owner(NativePluginPreferences* preferences,u64 owner) noexcept {
    if(!preferences || !owner || !native_plugin_preferences_valid(*preferences)) return false;
    for(u32 i=0;i<preferences->count;++i) if(preferences->entries[i].owner_id==owner) {
        for(u32 j=i+1;j<preferences->count;++j) preferences->entries[j-1]=preferences->entries[j];
        preferences->entries[--preferences->count]={};return true;
    }
    return false;
}
NativePluginPreferencesResult native_plugin_preferences_encode(const NativePluginPreferences& preferences,Slice<u8> record) noexcept {
    if(record.count!=kNativePluginPreferencesRecordBytes || !record.data || !native_plugin_preferences_valid(preferences) ||
        !disjoint({record.data,record.count},{reinterpret_cast<const u8*>(&preferences),sizeof(preferences)})) return fail(Error::invalid_input);
    clear_bytes(record.data,record.count);copy_bytes(record.data,kMagic,8);
    byte_store_le32(record.data+8,kNativePluginPreferencesVersion);byte_store_le32(record.data+12,record.count);
    byte_store_le32(record.data+16,preferences.count);
    for(u32 i=0;i<preferences.count;++i) {
        auto* row=record.data+32+i*kNativePluginPreferencesEntryBytes;const auto& key=preferences.entries[i];
        byte_store_le64(row,key.owner_id);byte_store_le32(row+8,key.abi_version);byte_store_le32(row+12,key.dll_bytes);
        copy_bytes(row+16,key.sha256,32);copy_bytes(row+48,key.dll,key.dll_bytes);
        byte_store_le32(row+176,key.permissions);byte_store_le32(row+180,key.flags);
    }
    byte_store_le64(record.data+24,checksum({record.data,record.count}));return {};
}
NativePluginPreferencesResult native_plugin_preferences_decode(ConstSlice<u8> record,NativePluginPreferences* preferences) noexcept {
    if(!preferences || !record.data || !disjoint(record,{reinterpret_cast<const u8*>(preferences),sizeof(*preferences)})) return fail(Error::invalid_input);
    if(record.count!=kNativePluginPreferencesRecordBytes) return fail(Error::size);
    if(!bytes_equal(record.data,kMagic,8) || byte_load_le32(record.data+12)!=record.count ||
        byte_load_le32(record.data+20)) return fail(Error::format);
    if(byte_load_le32(record.data+8)!=kNativePluginPreferencesVersion) return fail(Error::version);
    if(byte_load_le64(record.data+24)!=checksum(record)) return fail(Error::checksum);
    NativePluginPreferences candidate{};candidate.count=byte_load_le32(record.data+16);
    if(candidate.count>kNativePluginPreferencesMaximumEntries) return fail(Error::format);
    for(u32 i=0;i<kNativePluginPreferencesMaximumEntries;++i) {
        const auto* row=record.data+32+i*kNativePluginPreferencesEntryBytes;
        auto& key=candidate.entries[i];key.owner_id=byte_load_le64(row);key.abi_version=byte_load_le32(row+8);
        key.dll_bytes=byte_load_le32(row+12);copy_bytes(key.sha256,row+16,32);copy_bytes(key.dll,row+48,sizeof(key.dll));
        key.permissions=byte_load_le32(row+176);key.flags=byte_load_le32(row+180);
    }
    if(!native_plugin_preferences_valid(candidate)) return fail(Error::format);
    *preferences=candidate;return {};
}
NativePluginPreferencesResult native_plugin_preferences_release(NativePluginPreferencesIo* io) noexcept {
    if(!io) return fail(Error::invalid_input);
    if(!io->cleanup) return {};
    auto& held=*io->cleanup;
    if(held.file) {
        if(held.temporary) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if(!SetFileInformationByHandle(held.file,FileDispositionInfo,&disposition,sizeof(disposition))) return fail(Error::release,GetLastError());
            held.temporary=false;
        }
        if(!CloseHandle(held.file)) return fail(Error::release,GetLastError());held.file=nullptr;
    }
    while(held.ancestor_count) {
        HANDLE& ancestor=held.ancestors[held.ancestor_count-1];
        if(!CloseHandle(ancestor)) return fail(Error::release,GetLastError());ancestor=nullptr;--held.ancestor_count;
    }
    if(!VirtualFree(io->cleanup,0,MEM_RELEASE)) return fail(Error::release,GetLastError());
    io->cleanup=nullptr;return {};
}
NativePluginPreferencesResult native_plugin_preferences_load_file(NativePluginPreferencesIo* io,const wchar_t* path,NativePluginPreferences* preferences) noexcept {
    if(!preferences || !io || !disjoint({reinterpret_cast<const u8*>(preferences),sizeof(*preferences)},
        {reinterpret_cast<const u8*>(io),sizeof(*io)})) return fail(Error::invalid_input);
    u32 length=0,parent=0;
    if(!path_valid(path,&length,&parent) || !disjoint({reinterpret_cast<const u8*>(preferences),sizeof(*preferences)},
        {reinterpret_cast<const u8*>(path),(length+1)*2})) {*preferences={};return fail(Error::path);}
    *preferences={};auto outcome=begin(io,path,&parent);if(!outcome.ok()) return outcome;
    auto& held=*io->cleanup;outcome=pin(held,parent,false);
    if(!outcome.ok()) {
        if(outcome.error==Error::directory && (outcome.win32_error==ERROR_FILE_NOT_FOUND || outcome.win32_error==ERROR_PATH_NOT_FOUND)) outcome={Error::none,outcome.win32_error,true};
        return finish(io,outcome);
    }
    outcome=open_read(held);if(!outcome.ok() || outcome.missing) return finish(io,outcome);
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(held.file,&size) || size.QuadPart<12 || size.QuadPart>static_cast<LONGLONG>(sizeof(held.record)))
        return finish(io,fail(Error::size));
    const u32 bytes=static_cast<u32>(size.QuadPart);
    u32 error=0;
    if(!file_persistence_read_complete(held.file,{held.record,bytes},&error)) return finish(io,fail(Error::read,error));
    // A file written by another version of the format is not read; it counts as absent.
    if(bytes_equal(held.record,kMagic,8) && byte_load_le32(held.record+8)!=kNativePluginPreferencesVersion)
        return finish(io,{Error::none,0,true});
    outcome=native_plugin_preferences_decode({held.record,bytes},preferences);
    outcome=finish(io,outcome);if(!outcome.ok()) *preferences={};return outcome;
}
NativePluginPreferencesResult native_plugin_preferences_save_file(NativePluginPreferencesIo* io,const wchar_t* path,const NativePluginPreferences& preferences) noexcept {
    return save_file(io,path,preferences,false);
}
NativePluginPreferencesResult native_plugin_preferences_load(NativePluginPreferencesIo* io,NativePluginPreferences* preferences) noexcept {
    if(!preferences) return fail(Error::invalid_input);
    wchar_t path[1024]{};const auto made=user_path(path);
    if(!made.ok()) {*preferences={};return made;}return native_plugin_preferences_load_file(io,path,preferences);
}
NativePluginPreferencesResult native_plugin_preferences_save(NativePluginPreferencesIo* io,const NativePluginPreferences& preferences) noexcept {
    wchar_t path[1024]{};const auto made=user_path(path);
    return made.ok()?save_file(io,path,preferences,true):made;
}
} // namespace maicord

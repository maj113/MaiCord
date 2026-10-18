#include "maicord/app/plugin_preferences.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

// Test-only imported Win32 wrappers exercise the production source, with no
// shipping fault seam. No DLL is created or loaded by these fixtures.
namespace maicord::preference_fault_test {
enum class Fault : u8 { none, close_file, close_directory, free, disposition, rename, flush, postcommit_flush, reparse, alias, collision, collisions };
Fault fault{};
u32 flushes{};
u32 create_attempts{};
wchar_t last_temporary[1024]{};
HANDLE WINAPI create(const wchar_t* path,DWORD access,DWORD share,SECURITY_ATTRIBUTES* security,DWORD mode,DWORD attributes,HANDLE template_file) noexcept {
    if(mode==CREATE_NEW) {
        ++create_attempts;
        u32 i=0;for(;i<1023 && path[i];++i) last_temporary[i]=path[i];last_temporary[i]=0;
        if(fault==Fault::collision || fault==Fault::collisions) {
            if(fault==Fault::collision)fault=Fault::none;
            SetLastError(ERROR_FILE_EXISTS);return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFileW(path,access,share,security,mode,attributes,template_file);
}
BOOL WINAPI close(HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    const bool disk=GetFileInformationByHandle(handle,&info)!=0;
    const bool directory=disk && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if((fault==Fault::close_file && disk && !directory) || (fault==Fault::close_directory && directory)) {
        fault=Fault::none;SetLastError(ERROR_GEN_FAILURE);return FALSE;
    }
    return CloseHandle(handle);
}
BOOL WINAPI free(void* address,SIZE_T bytes,DWORD kind) noexcept {
    if(fault==Fault::free) {fault=Fault::none;SetLastError(ERROR_GEN_FAILURE);return FALSE;}
    return VirtualFree(address,bytes,kind);
}
BOOL WINAPI information(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,void* data,DWORD bytes) noexcept {
    if((fault==Fault::disposition && kind==FileDispositionInfo) || (fault==Fault::rename && kind==FileRenameInfo)) {
        fault=Fault::none;SetLastError(ERROR_GEN_FAILURE);return FALSE;
    }
    return SetFileInformationByHandle(handle,kind,data,bytes);
}
BOOL WINAPI flush(HANDLE handle) noexcept {
    ++flushes;
    if(fault==Fault::flush || (fault==Fault::postcommit_flush && flushes==2)) {
        fault=Fault::none;SetLastError(ERROR_GEN_FAILURE);return FALSE;
    }
    return FlushFileBuffers(handle);
}
BOOL WINAPI attributes(HANDLE handle,BY_HANDLE_FILE_INFORMATION* info) noexcept {
    const BOOL result=GetFileInformationByHandle(handle,info);
    if(result && fault==Fault::reparse) {fault=Fault::none;info->dwFileAttributes|=FILE_ATTRIBUTE_REPARSE_POINT;}
    return result;
}
DWORD WINAPI final_path(HANDLE handle,wchar_t* path,DWORD count,DWORD flags) noexcept {
    const DWORD result=GetFinalPathNameByHandleW(handle,path,count,flags);
    if(result && result<count && fault==Fault::alias) {fault=Fault::none;path[4]=path[4]==L'Z'?L'Y':L'Z';}
    return result;
}
} // namespace maicord::preference_fault_test

// The production source references these before their definitions.
namespace maicord {
bool fault_preferences_valid(const NativePluginPreferences&) noexcept;
NativePluginPreferencesResult fault_preferences_release(NativePluginPreferencesIo*) noexcept;
NativePluginPreferencesResult fault_preferences_encode(const NativePluginPreferences&,Slice<u8>) noexcept;
NativePluginPreferencesResult fault_preferences_decode(ConstSlice<u8>,NativePluginPreferences*) noexcept;
}
#define CloseHandle maicord::preference_fault_test::close
#define CreateFileW maicord::preference_fault_test::create
#define VirtualFree maicord::preference_fault_test::free
#define SetFileInformationByHandle maicord::preference_fault_test::information
#define FlushFileBuffers maicord::preference_fault_test::flush
#define GetFileInformationByHandle maicord::preference_fault_test::attributes
#define GetFinalPathNameByHandleW maicord::preference_fault_test::final_path
#define native_plugin_preferences_valid fault_preferences_valid
#define native_plugin_preferences_find fault_preferences_find
#define native_plugin_preferences_restorable fault_preferences_restorable
#define native_plugin_preferences_set_flags fault_preferences_set_flags
#define native_plugin_preferences_enable fault_preferences_enable
#define native_plugin_preferences_remove_owner fault_preferences_remove_owner
#define native_plugin_preferences_encode fault_preferences_encode
#define native_plugin_preferences_decode fault_preferences_decode
#define native_plugin_preferences_release fault_preferences_release
#define native_plugin_preferences_load_file fault_preferences_load_file
#define native_plugin_preferences_save_file fault_preferences_save_file
#define native_plugin_preferences_load fault_preferences_load
#define native_plugin_preferences_save fault_preferences_save
#include "../src/app/plugin_preferences.cpp"
#undef CloseHandle
#undef CreateFileW
#undef VirtualFree
#undef SetFileInformationByHandle
#undef FlushFileBuffers
#undef GetFileInformationByHandle
#undef GetFinalPathNameByHandleW
#undef native_plugin_preferences_valid
#undef native_plugin_preferences_find
#undef native_plugin_preferences_restorable
#undef native_plugin_preferences_set_flags
#undef native_plugin_preferences_enable
#undef native_plugin_preferences_remove_owner
#undef native_plugin_preferences_encode
#undef native_plugin_preferences_decode
#undef native_plugin_preferences_release
#undef native_plugin_preferences_load_file
#undef native_plugin_preferences_save_file
#undef native_plugin_preferences_load
#undef native_plugin_preferences_save

namespace maicord {
namespace {
NativePluginManifest preferences_fixture_manifest(u64 owner=42) noexcept {
    NativePluginManifest manifest{};manifest.owner_id=owner;manifest.abi_version=5;
    copy_bytes(manifest.name,"Fixture",7);manifest.name_bytes=7;
    manifest.version[0]='1';manifest.version_bytes=1;
    copy_bytes(manifest.dll,"Fixture.DLL",11);manifest.dll_bytes=11;
    for(u32 i=0;i<32;++i) manifest.sha256[i]=static_cast<u8>(i+1);
    return manifest;
}
void preferences_fixture_checksum(u8* record) noexcept {
    byte_store_le64(record+24,hash64_bytes(hash64_bytes(kHash64Seed,record,24),record+32,kNativePluginPreferencesRecordBytes-32));
}
bool preferences_fixture_write(const wchar_t* path,const u8* bytes,u32 count) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD written=0;const bool result=WriteFile(file,bytes,count,&written,nullptr) && written==count;
    return CloseHandle(file) && result;
}
bool preferences_fixture_committed(void* page) noexcept {
    MEMORY_BASIC_INFORMATION info{};return page && VirtualQuery(page,&info,sizeof(info))==sizeof(info) &&
        info.State==MEM_COMMIT && info.AllocationBase==page && info.RegionSize==4096;
}
}
void test_plugin_preferences(void (*check)(bool,const char*) noexcept) noexcept {
    NativePluginPreferences preferences{},decoded{};auto manifest=preferences_fixture_manifest();
    check(native_plugin_preferences_valid(preferences) && !native_plugin_preferences_find(preferences,manifest),
        "plugin preferences: zero-init has no implicit execution authority");
    check(native_plugin_preferences_enable(&preferences,manifest).ok() && preferences.count==1 &&
        native_plugin_preferences_find(preferences,manifest) && preferences.entries[0].dll[0]=='f',
        "plugin preferences: successful explicit choice stores canonical ASCII identity");
    auto changed=manifest;changed.sha256[0]^=1;
    check(!native_plugin_preferences_find(preferences,changed),"plugin preferences: hash changes require a new explicit Enable");
    changed=manifest;changed.owner_id++;
    check(!native_plugin_preferences_find(preferences,changed),"plugin preferences: owner change never reuses authority");
    changed=manifest;changed.abi_version++;
    check(!native_plugin_preferences_find(preferences,changed),"plugin preferences: unsupported ABI cannot match authority");
    changed=manifest;changed.dll[0]='X';
    check(!native_plugin_preferences_find(preferences,changed),"plugin preferences: DLL leaf changes require explicit Enable");
    changed=manifest;changed.dll[0]='f';changed.version[0]='2';
    check(native_plugin_preferences_find(preferences,changed)!=nullptr,"plugin preferences: DLL case and presentation version do not change code identity");
    const auto sentinel=preferences;
    changed=manifest;changed.owner_id++;
    check(native_plugin_preferences_enable(&preferences,changed).error==NativePluginPreferencesError::invalid_input &&
        bytes_equal(&preferences,&sentinel,sizeof(preferences)),"plugin preferences: duplicate leaf owners preserve choices on failure");
    u8 record[kNativePluginPreferencesRecordBytes+1]{};
    check(native_plugin_preferences_encode(preferences,{record,kNativePluginPreferencesRecordBytes}).ok() && record[8]==2 &&
        byte_load_le32(record+12)==kNativePluginPreferencesRecordBytes && byte_load_le64(record+32)==42 &&
        native_plugin_preferences_decode({record,kNativePluginPreferencesRecordBytes},&decoded).ok() &&
        native_plugin_preferences_find(decoded,manifest),"plugin preferences: strict little endian v2 record round trips exact identity");
    const auto reject=[&](NativePluginPreferencesError expected,u32 count=kNativePluginPreferencesRecordBytes) noexcept {
        decoded=sentinel;const auto result=native_plugin_preferences_decode({record,count},&decoded);
        return result.error==expected && bytes_equal(&decoded,&sentinel,sizeof(decoded));
    };
    const auto reset=[&]() noexcept {return native_plugin_preferences_encode(sentinel,{record,kNativePluginPreferencesRecordBytes}).ok();};
    check(reject(NativePluginPreferencesError::size,kNativePluginPreferencesRecordBytes-1) &&
        reject(NativePluginPreferencesError::size,kNativePluginPreferencesRecordBytes+1),
        "plugin preferences: truncated and trailing encodings preserve output");
    record[0]^=1;check(reject(NativePluginPreferencesError::format),"plugin preferences: bad magic fails closed");
    reset();record[8]=3;check(reject(NativePluginPreferencesError::version),"plugin preferences: future record version fails closed");
    reset();record[20]=1;check(reject(NativePluginPreferencesError::format),"plugin preferences: reserved header rejects hidden fields");
    reset();record[48]^=1;check(reject(NativePluginPreferencesError::checksum),"plugin preferences: corrupted digest cannot grant trust");
    reset();record[24]^=1;check(reject(NativePluginPreferencesError::checksum),"plugin preferences: damaged checksum fails closed");
    reset();record[16]=9;preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: checksum-valid oversized choice count is rejected");
    reset();record[32+kNativePluginPreferencesEntryBytes]=1;preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: unused slots must be entirely zero");
    reset();copy_bytes(record+32+kNativePluginPreferencesEntryBytes,record+32,kNativePluginPreferencesEntryBytes);record[16]=2;preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: duplicate owners cannot survive a valid checksum");
    byte_store_le64(record+32+kNativePluginPreferencesEntryBytes,43);preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: duplicate canonical leaves cannot survive a valid checksum");
    reset();record[32+48]='F';preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: persisted leaves must be canonical lowercase");
    reset();record[32+48+11]='x';preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: DLL padding cannot carry unknown bytes");
    reset();record[32+8]=6;preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: checksum-valid unsupported ABI fails closed");
    reset();record[32+8]=4;preferences_fixture_checksum(record);
    check(reject(NativePluginPreferencesError::format),"plugin preferences: the retired ABI 4 identity is no longer accepted");
    for(u32 i=1;i<8;++i) {
        changed=manifest;changed.owner_id=42+i;changed.dll[0]=static_cast<char>('0'+i);
        check(native_plugin_preferences_enable(&preferences,changed).ok(),"plugin preferences: all eight bounded choices fit");
    }
    changed=manifest;changed.owner_id=99;changed.dll[0]='z';const auto full=preferences;
    check(native_plugin_preferences_enable(&preferences,changed).error==NativePluginPreferencesError::capacity &&
        bytes_equal(&full,&preferences,sizeof(full)),"plugin preferences: ninth choice refuses without eviction");
    changed=manifest;changed.sha256[0]^=1;
    check(native_plugin_preferences_enable(&preferences,changed).ok() && preferences.count==8 &&
        native_plugin_preferences_find(preferences,changed) && !native_plugin_preferences_find(preferences,manifest),
        "plugin preferences: explicit owner update revokes the previous digest even at capacity");
    check(native_plugin_preferences_remove_owner(&preferences,42) && !native_plugin_preferences_find(preferences,changed) &&
        native_plugin_preferences_valid(preferences) && preferences.count==7,
        "plugin preferences: Disable removes owner identity and clears vacated storage");
    check(native_plugin_preferences_encode(sentinel,{reinterpret_cast<u8*>(const_cast<NativePluginPreferences*>(&sentinel)),1440}).error==NativePluginPreferencesError::invalid_input &&
        native_plugin_preferences_decode({reinterpret_cast<const u8*>(&decoded),1440},&decoded).error==NativePluginPreferencesError::invalid_input,
        "plugin preferences: overlapped records are rejected before read or mutation");

    wchar_t temp[MAX_PATH]{},path[MAX_PATH]{},sibling[MAX_PATH+4]{};
    const DWORD count=GetTempPathW(MAX_PATH,temp);
    const bool fixture=count && count<MAX_PATH && GetTempFileNameW(temp,L"mpp",0,path) && DeleteFileW(path);
    check(fixture,"plugin preferences: isolated offline fixture creates no plugin code");if(!fixture)return;
    u32 length=0;while(path[length]) {sibling[length]=path[length];++length;}
    sibling[length]=L'.';sibling[length+1]=L'n';sibling[length+2]=L'e';sibling[length+3]=L'w';
    struct Cleanup {const wchar_t* path;const wchar_t* sibling;~Cleanup() noexcept {DeleteFileW(path);DeleteFileW(sibling);}} cleanup{path,sibling};
    NativePluginPreferencesIo io{};
    decoded=sentinel;auto loaded=native_plugin_preferences_load_file(&io,path,&decoded);
    check(loaded.ok() && loaded.missing && !decoded.count && !io.cleanup,"plugin preferences: missing file succeeds with no authority or retained IO");
    auto saved=native_plugin_preferences_save_file(&io,path,sentinel);
    check(saved.ok() && saved.committed && !io.cleanup && native_plugin_preferences_load_file(&io,path,&decoded).ok() &&
        native_plugin_preferences_find(decoded,manifest),"plugin preferences: exclusive retained-handle atomic save round trips");
    check(native_plugin_preferences_save_file(&io,path,preferences).ok() && native_plugin_preferences_load_file(&io,path,&decoded).ok() &&
        decoded.count==7,"plugin preferences: atomic replacement stores a complete edited record");
    HANDLE locked=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(locked!=INVALID_HANDLE_VALUE,"plugin preferences: fixture locks destination against replacement");
    if(locked!=INVALID_HANDLE_VALUE) {
        saved=native_plugin_preferences_save_file(&io,path,sentinel);
        check(saved.error==NativePluginPreferencesError::commit && !saved.committed && !io.cleanup &&
            GetFileAttributesW(sibling)==INVALID_FILE_ATTRIBUTES,"plugin preferences: locked replacement preserves old file and deletes only owned sibling");
        CloseHandle(locked);
        check(native_plugin_preferences_load_file(&io,path,&decoded).ok() && decoded.count==7,
            "plugin preferences: failed commit leaves durable choices unchanged");
    }
    reset();check(preferences_fixture_write(sibling,record,3),"plugin preferences: preexisting sibling fixture exists");
    saved=native_plugin_preferences_save_file(&io,path,sentinel);
    HANDLE existing=CreateFileW(sibling,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);LARGE_INTEGER size{};
    check(saved.ok() && saved.committed && !io.cleanup && existing!=INVALID_HANDLE_VALUE &&
        GetFileSizeEx(existing,&size) && size.QuadPart==3,"plugin preferences: stale .new never blocks save or gets overwritten or cleaned up");
    if(existing!=INVALID_HANDLE_VALUE)CloseHandle(existing);DeleteFileW(sibling);
    check(native_plugin_preferences_save_file(&io,path,preferences).ok(),"plugin preferences: collision recovery preserves later normal saves");
    preference_fault_test::fault=preference_fault_test::Fault::collision;preference_fault_test::create_attempts=0;
    saved=fault_preferences_save_file(&io,path,preferences);
    check(saved.ok() && saved.committed && preference_fault_test::create_attempts==2 && !io.cleanup &&
        GetFileAttributesW(preference_fault_test::last_temporary)==INVALID_FILE_ATTRIBUTES,
        "plugin preferences: unique transaction retries CREATE_NEW collision without scanning or touching unknown files");
    preference_fault_test::fault=preference_fault_test::Fault::collisions;preference_fault_test::create_attempts=0;
    saved=fault_preferences_save_file(&io,path,sentinel);preference_fault_test::fault=preference_fault_test::Fault::none;
    check(saved.error==NativePluginPreferencesError::open && !saved.committed && preference_fault_test::create_attempts==16 && !io.cleanup &&
        native_plugin_preferences_load_file(&io,path,&decoded).ok() && decoded.count==7,
        "plugin preferences: exhaustive collisions stop at sixteen and preserve prior file");
    decoded=sentinel;check(preferences_fixture_write(path,record,kNativePluginPreferencesRecordBytes-1) &&
        native_plugin_preferences_load_file(&io,path,&decoded).error==NativePluginPreferencesError::size && !decoded.count && !io.cleanup,
        "plugin preferences: truncated file clears all runtime authority");
    record[48]^=1;decoded=sentinel;
    check(preferences_fixture_write(path,record,kNativePluginPreferencesRecordBytes) && native_plugin_preferences_load_file(&io,path,&decoded).error==NativePluginPreferencesError::checksum &&
        !decoded.count && !io.cleanup,"plugin preferences: corrupted file clears all runtime authority");
    check(native_plugin_preferences_save_file(&io,path,sentinel).ok(),"plugin preferences: explicit edit repairs corrupt trust file");
    constexpr const wchar_t* invalid_paths[]{L"relative.bin",L"\\\\?\\C:\\trust.bin",L"\\\\server\\share\\trust.bin",L"C:/trust.bin",
        L"C:\\..\\trust.bin",L"C:\\folder.\\trust.bin",L"C:\\FOLDER~1\\trust.bin",L"C:\\con.bin",L"C:\\trust.bin:stream"};
    for(auto invalid_path:invalid_paths) {
        decoded=sentinel;check(native_plugin_preferences_load_file(&io,invalid_path,&decoded).error==NativePluginPreferencesError::path &&
            !decoded.count && !io.cleanup,"plugin preferences: alias/device/relative paths are refused before IO");
    }
    using namespace preference_fault_test;
    const Fault load_faults[]{Fault::close_file,Fault::close_directory,Fault::free,Fault::reparse,Fault::alias};
    for(auto injected:load_faults) {
        fault=injected;decoded=sentinel;loaded=fault_preferences_load_file(&io,path,&decoded);void* page=io.cleanup;
        const bool release_failure=injected==Fault::close_file || injected==Fault::close_directory || injected==Fault::free;
        check(!loaded.ok() && !decoded.count && (release_failure ? loaded.error==NativePluginPreferencesError::release && preferences_fixture_committed(page):!page),
            "plugin preferences: injected close/free/reparse/alias failure never leaves runtime trust");
        if(page) {
            check(fault_preferences_load_file(&io,path,&decoded).error==NativePluginPreferencesError::busy,
                "plugin preferences: retained cleanup refuses implicit reuse");
            check(fault_preferences_release(&io).ok() && !io.cleanup && !preferences_fixture_committed(page),
                "plugin preferences: explicit release retry drains retained handles and page");
        }
    }
    check(DeleteFileW(path),"plugin preferences: new-file save fault fixture starts missing");
    fault=Fault::close_file;flushes=0;saved=fault_preferences_save_file(&io,path,sentinel);void* page=io.cleanup;
    check(saved.error==NativePluginPreferencesError::release && saved.committed && preferences_fixture_committed(page),
        "plugin preferences: postcommit close failure reports committed and retains retryable ownership");
    check(fault_preferences_release(&io).ok() && !io.cleanup && native_plugin_preferences_load_file(&io,path,&decoded).ok() &&
        native_plugin_preferences_find(decoded,manifest),"plugin preferences: committed record survives release retry");
    fault=Fault::postcommit_flush;flushes=0;saved=fault_preferences_save_file(&io,path,preferences);
    check(saved.error==NativePluginPreferencesError::flush && saved.committed && !io.cleanup &&
        native_plugin_preferences_load_file(&io,path,&decoded).ok() && decoded.count==7,
        "plugin preferences: postcommit flush failure distinguishes new durable identity from unchanged destination");
    const Fault save_faults[]{Fault::rename,Fault::flush,Fault::disposition};
    for(auto injected:save_faults) {
        fault=injected;flushes=0;
        // disposition is reached after an intentionally blocked rename.
        HANDLE block=injected==Fault::disposition?CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr):INVALID_HANDLE_VALUE;
        saved=fault_preferences_save_file(&io,path,sentinel);page=io.cleanup;
        check(!saved.ok() && !saved.committed && (injected==Fault::disposition ? page!=nullptr:page==nullptr),
            "plugin preferences: precommit write/rename/cleanup failures never claim committed state");
        if(block!=INVALID_HANDLE_VALUE)CloseHandle(block);
        if(page) check(fault_preferences_release(&io).ok() && !io.cleanup,"plugin preferences: disposition failure retains owned temporary until explicit retry");
        check(GetFileAttributesW(last_temporary)==INVALID_FILE_ATTRIBUTES && native_plugin_preferences_load_file(&io,path,&decoded).ok() && decoded.count==7,
            "plugin preferences: failed atomic edits retain complete prior record and no stray owned sibling");
    }
    fault=Fault::none;check(native_plugin_preferences_release(&io).ok() && !io.cleanup,"plugin preferences: empty cleanup release is idempotent");
}
} // namespace maicord

#include "maicord/app/plugin_bundle_install.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include <stddef.h>

#include <Windows.h>

namespace maicord {
namespace {
using Error=NativePluginBundleInstallError;
using Phase=NativePluginBundleInstallPhase;
#if defined(MAICORD_PLUGIN_BUNDLE_INSTALL_FIXTURE)
volatile LONG failure_mask{};
bool forced(u32 mask) noexcept {return (static_cast<u32>(InterlockedCompareExchange(&failure_mask,0,0))&mask)!=0;}
#else
constexpr bool forced(u32) noexcept {return false;}
#endif
struct Runtime {
    NativePluginBundleInstall* owner{};
    NativePluginBundleInstallResult result{};
    NativePluginBundleInstallResources resources{};
    NativePluginManifest source{},installed{},old{};
    NativePluginCatalog catalog{};
    NativePluginCatalogEntry entries[kNativePluginCatalogMaximumEntries]{};
    NativePluginVerifiedFile source_directory{},destination_directory{},source_manifest{},source_dll{},
        target_manifest{},target_dll{},staged_manifest{},lock{},retired_dll{},validation{};
    wchar_t input[kNativePluginCatalogPathCapacity]{},destination[kNativePluginCatalogPathCapacity]{},
        manifest_leaf[kNativePluginCatalogFilenameCapacity]{};
    u32 input_characters{},destination_characters{},manifest_characters{},record_bytes{};
    char record[kNativePluginManifestMaximumBytes]{};
    u8 copy_scratch[8192]{};
    BY_HANDLE_FILE_INFORMATION old_identity{},retired_identity{};
    volatile LONG wait_failed{};
    // Set only by the catalog start functions, so local-bundle-only builds never link them.
    Error (*after_commit)(Runtime&){};
    u8 keep[2][32]{},candidates[8][32]{};
    u32 keep_count{};
    bool created_dll{},staged_file{},retire_old{},old_deleted{},cleanup_only{},catalog_ready{},retain_previous{},after_done{},abandoned{};
};
static_assert(sizeof(Runtime)<=96*1024,"Plugin bundle staging runtime must stay within 96 KiB");
LONG phase_value(Phase phase) noexcept {return static_cast<LONG>(phase);}
Phase phase(const NativePluginBundleInstall& owner) noexcept {
    return static_cast<Phase>(InterlockedCompareExchange(const_cast<volatile LONG*>(&owner.phase),0,0));
}
bool cancelled(const Runtime& r) noexcept {return InterlockedCompareExchange(&r.owner->cancelled,0,0)!=0;}
Error fail(Runtime& r,Error error,NativePluginCatalogResult detail={}) noexcept {r.result.error=error;r.result.filesystem=detail;return error;}
bool same_dll(const NativePluginManifest& a,const NativePluginManifest& b) noexcept {
    if(a.dll_bytes!=b.dll_bytes)return false;
    for(u32 i=0;i<a.dll_bytes;++i)if(ascii_lower(a.dll[i])!=ascii_lower(b.dll[i]))return false;
    return true;
}
bool same_manifest(const NativePluginManifest& a,const NativePluginManifest& b) noexcept {
    return a.owner_id==b.owner_id&&a.abi_version==b.abi_version&&same_dll(a,b)&&bytes_equal(a.sha256,b.sha256,32)&&
        a.name_bytes==b.name_bytes&&bytes_equal(a.name,b.name,a.name_bytes)&&
        a.version_bytes==b.version_bytes&&bytes_equal(a.version,b.version,a.version_bytes)&&
        a.description_bytes==b.description_bytes&&bytes_equal(a.description,b.description,a.description_bytes)&&
        a.permissions==b.permissions&&a.min_host_abi==b.min_host_abi&&
        a.publisher_bytes==b.publisher_bytes&&bytes_equal(a.publisher,b.publisher,a.publisher_bytes);
}
void hexadecimal(u64 value,wchar_t* out) noexcept {
    constexpr wchar_t digits[]=L"0123456789abcdef";
    for(u32 i=0;i<16;++i)out[i]=digits[(value>>((15-i)*4))&15];
}
void managed_name(const NativePluginManifest& manifest,wchar_t (&name)[128]) noexcept {
    hexadecimal(manifest.owner_id,name);name[16]=L'-';constexpr wchar_t digits[]=L"0123456789abcdef";
    for(u32 i=0;i<32;++i){name[17+i*2]=digits[manifest.sha256[i]>>4];name[18+i*2]=digits[manifest.sha256[i]&15];}
    name[81]=L'.';name[82]=L'd';name[83]=L'l';name[84]=L'l';name[85]=0;
}
bool managed(const NativePluginManifest& manifest) noexcept {
    if(manifest.dll_bytes!=85)return false;wchar_t name[128]{};managed_name(manifest,name);
    for(u32 i=0;i<85;++i)if(ascii_lower(manifest.dll[i])!=name[i])return false;
    return true;
}
bool source_path(Runtime& r,u32* directory,u32* leaf) noexcept {
    if(r.input_characters<7)return false;
    u32 slash{};for(u32 i=0;i<r.input_characters;++i)if(r.input[i]==L'\\')slash=i;
    if(slash<2 || slash+1>=r.input_characters || r.input_characters-slash-1>=128)return false;
    *directory=slash==2?3:slash;*leaf=slash+1;return true;
}
bool destination_valid(const Runtime& r) noexcept {
    constexpr wchar_t suffix[]=L"\\MaiCord\\plugins";constexpr u32 n=sizeof(suffix)/sizeof(wchar_t)-1;
    if(r.destination_characters<n+2)return false;
    for(u32 i=0;i<n;++i)if(ascii_lower(r.destination[r.destination_characters-n+i])!=ascii_lower(suffix[i]))return false;
    return true;
}
u32 handles(const NativePluginVerifiedFile& file) noexcept {
    return file.ancestor_count+u32(file.file!=nullptr)+u32(file.scan!=nullptr)+u32(file.hash!=nullptr)+u32(file.algorithm!=nullptr);
}
void checkpoint(Runtime& r) noexcept {
    u32 count=3;
    const NativePluginVerifiedFile* gates[]{&r.source_directory,&r.destination_directory,&r.source_manifest,&r.source_dll,
        &r.target_manifest,&r.target_dll,&r.staged_manifest,&r.lock,&r.retired_dll,&r.validation};
    for(const auto* gate:gates)count+=handles(*gate);
    if(r.catalog.cleanup)count+=handles(*r.catalog.cleanup);
    r.resources.attached_handles=count;
    r.resources.peak_checkpoint_handles=maximum(r.resources.peak_checkpoint_handles,count);
}
bool final_path_equal(const NativePluginVerifiedFile& file) noexcept {
    wchar_t path[kNativePluginCatalogPathCapacity+4]{};
    const DWORD count=GetFinalPathNameByHandleW(file.file,path,kNativePluginCatalogPathCapacity+4,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(count!=file.path_characters+4||path[0]!=L'\\'||path[1]!=L'\\'||path[2]!=L'?'||path[3]!=L'\\')return false;
    for(u32 i=0;i<file.path_characters;++i)if(ascii_lower(path[i+4])!=ascii_lower(file.path[i]))return false;
    return true;
}
bool identity_equal(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow;
}
bool manifest_still_expected(Runtime& r) noexcept {
    if(!final_path_equal(r.target_manifest))return false;
    NativePluginManifest manifest{};
    const auto read=native_plugin_pinned_manifest_read(r.destination_directory,{r.manifest_leaf,r.manifest_characters},&manifest,&r.validation);
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool matches=read.ok()&&GetFileInformationByHandle(r.validation.file,&identity)&&identity_equal(identity,r.old_identity)&&same_manifest(manifest,r.old);
    const auto released=native_plugin_verified_file_release(&r.validation);
    if(!released.ok())return false;
    return matches;
}
Error inventory(Runtime& r) noexcept {
    r.catalog_ready=false;
    auto scanned=native_plugin_catalog_release(&r.catalog);
    if(!scanned.ok())return fail(r,Error::release,scanned);
    scanned=native_plugin_catalog_initialize(&r.catalog,{r.destination_directory.path,r.destination_directory.path_characters},{r.entries,32});
    if(scanned.ok())scanned=native_plugin_catalog_refresh(&r.catalog);
    checkpoint(r);
    if(!scanned.ok())return fail(r,Error::catalog,scanned);
    r.catalog_ready=true;return Error::none;
}
bool other_reference(const Runtime& r,const NativePluginManifest& manifest) noexcept {
    for(u32 i=0;i<r.catalog.count;++i){const auto& entry=r.catalog.entries[i];
        if(entry.filename_characters==r.manifest_characters){bool same=true;
            for(u32 j=0;j<r.manifest_characters;++j)if(ascii_lower(entry.filename[j])!=ascii_lower(r.manifest_leaf[j]))same=false;
            if(same)continue;}
        // Unknown malformed metadata cannot establish a negative reference.
        if(entry.status!=NativePluginCatalogError::none || same_dll(entry.manifest,manifest))return true;
    }
    return false;
}
Error retention_limit(Runtime& r,bool new_file) noexcept {
    auto& directory=r.destination_directory;const u32 base=directory.path_characters;
    if(base+3>=kNativePluginCatalogPathCapacity)return fail(r,Error::path);
    wchar_t pattern[kNativePluginCatalogPathCapacity]{};
    for(u32 i=0;i<base;++i)pattern[i]=directory.path[i];pattern[base]=L'\\';pattern[base+1]=L'*';
    WIN32_FIND_DATAW data{};directory.scan=FindFirstFileExW(pattern,FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(directory.scan==INVALID_HANDLE_VALUE){directory.scan=nullptr;return fail(r,Error::catalog,{NativePluginCatalogError::scan,GetLastError()});}
    u32 visited{},count{};u64 bytes{};Error error=Error::none;wchar_t prefix[16];hexadecimal(r.installed.owner_id,prefix);
    do{
        if((data.cFileName[0]==L'.'&&!data.cFileName[1])||(data.cFileName[0]==L'.'&&data.cFileName[1]==L'.'&&!data.cFileName[2]))continue;
        if(++visited>kNativePluginCatalogMaximumDirectoryEntries){error=Error::catalog;break;}
        if(cancelled(r)){error=Error::cancelled;break;}
        u32 length{};while(length<MAX_PATH&&data.cFileName[length])++length;
        if(length!=85||data.cFileName[16]!=L'-'||data.cFileName[81]!=L'.'||ascii_lower(data.cFileName[82])!=L'd'||
            ascii_lower(data.cFileName[83])!=L'l'||ascii_lower(data.cFileName[84])!=L'l')continue;
        bool match=true;for(u32 i=0;i<16;++i)if(ascii_lower(data.cFileName[i])!=prefix[i])match=false;
        for(u32 i=17;i<81;++i){const auto c=ascii_lower(data.cFileName[i]);if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))match=false;}
        if(!match)continue;
        const u64 size=(u64{data.nFileSizeHigh}<<32)|data.nFileSizeLow;
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)||size>kNativePluginDllMaximumBytes){error=Error::conflict;break;}
        ++count;bytes+=size;
    }while(FindNextFileW(directory.scan,&data));
    if(error==Error::none&&GetLastError()!=ERROR_NO_MORE_FILES)error=Error::catalog;
    if(!FindClose(directory.scan))return fail(r,Error::release,{NativePluginCatalogError::release,GetLastError()});
    directory.scan=nullptr;
    if(error!=Error::none)return fail(r,error);
    // Retaining the previous version allows a third file while an update is staged.
    const u32 limit=r.retain_previous?3:2;
    if(count>limit || (new_file&&count>=limit) || bytes+(new_file?r.source_dll.bytes:0)>limit*kNativePluginDllMaximumBytes)
        return fail(r,Error::retained_limit);
    return Error::none;
}
// The cooperative per-owner lock shared by install, uninstall and recovery.
Error lock_owner(Runtime& r,u64 owner_id) noexcept {
    wchar_t lock[128]{};constexpr wchar_t lock_prefix[]=L"maicord-install-";
    for(u32 i=0;i<15;++i)lock[i]=lock_prefix[i];hexadecimal(owner_id,lock+15);
    constexpr wchar_t lock_suffix[]=L".lock";for(u32 i=0;i<6;++i)lock[31+i]=lock_suffix[i];
    const auto checked=native_plugin_pinned_file_open(r.destination_directory,{lock,36},&r.lock,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,
        CREATE_NEW,FILE_FLAG_DELETE_ON_CLOSE|FILE_ATTRIBUTE_TEMPORARY);
    checkpoint(r);if(!checked.ok())return fail(r,checked.win32_error==ERROR_FILE_EXISTS||checked.win32_error==ERROR_ALREADY_EXISTS||
        checked.win32_error==ERROR_SHARING_VIOLATION||checked.win32_error==ERROR_ACCESS_DENIED?Error::busy:Error::open,checked);
    return Error::none;
}
Error stage(Runtime& r) noexcept {
    u32 directory{},leaf{};
    if(!source_path(r,&directory,&leaf)||!destination_valid(r))return fail(r,Error::path);
    auto checked=native_plugin_directory_pin({r.input,directory},&r.source_directory);
    if(!checked.ok())return fail(r,Error::path,checked);
    checked=native_plugin_pinned_manifest_read(r.source_directory,{r.input+leaf,r.input_characters-leaf},&r.source,&r.source_manifest);
    checkpoint(r);if(!checked.ok())return fail(r,Error::manifest,checked);
    r.result.owner_id=r.source.owner_id;r.installed=r.source;
    wchar_t name[128]{};managed_name(r.source,name);clear_bytes(r.installed.dll,sizeof(r.installed.dll));
    for(u32 i=0;i<85;++i)r.installed.dll[i]=static_cast<char>(name[i]);r.installed.dll_bytes=85;
    checked=native_plugin_directory_pin({r.destination,r.destination_characters},&r.destination_directory,2);
    checkpoint(r);if(!checked.ok())return fail(r,Error::path,checked);
    if(lock_owner(r,r.source.owner_id)!=Error::none)return r.result.error;
    if(inventory(r)!=Error::none)return r.result.error;
    u32 found=~u32{0};
    for(u32 i=0;i<r.catalog.count;++i)if(r.catalog.entries[i].manifest.owner_id==r.source.owner_id){
        if(found!=~u32{0}||r.catalog.entries[i].status!=NativePluginCatalogError::none)return fail(r,Error::conflict);found=i;}
    if(found!=~u32{0}){
        const auto& entry=r.catalog.entries[found];r.manifest_characters=entry.filename_characters;
        for(u32 i=0;i<=r.manifest_characters;++i)r.manifest_leaf[i]=entry.filename[i];
        // POSIX replacement preserves the old file's handle, but requires
        // its pin to share deletion. Content/path/identity are rechecked before
        // the file becomes visible; only cooperating installers are serialized by our lock.
        checked=native_plugin_pinned_manifest_read(r.destination_directory,{r.manifest_leaf,r.manifest_characters},&r.old,&r.target_manifest,
            FILE_SHARE_READ|FILE_SHARE_DELETE);
        if(!checked.ok()||!same_manifest(r.old,entry.manifest)||!GetFileInformationByHandle(r.target_manifest.file,&r.old_identity))return fail(r,Error::conflict,checked);
        BY_HANDLE_FILE_INFORMATION source_identity{};
        if(!GetFileInformationByHandle(r.source_manifest.file,&source_identity))
            return fail(r,Error::conflict,{NativePluginCatalogError::file,GetLastError()});
        if(identity_equal(source_identity,r.old_identity)){
            // Selecting the installed legacy manifest can promote its DLL to
            // the managed leaf. The target pin preserves those same
            // source bytes without a redundant READ-only pin blocking rename.
            checked=native_plugin_verified_file_release(&r.source_manifest);
            if(!checked.ok())return fail(r,Error::release,checked);
        }
        r.result.update=true;r.result.idempotent=same_manifest(r.old,r.installed);
        r.retire_old=!r.retain_previous&&managed(r.old)&&!same_dll(r.old,r.installed);
        if(r.retire_old&&other_reference(r,r.old))return fail(r,Error::conflict);
    }else{
        if(r.catalog.count==kNativePluginCatalogMaximumEntries)return fail(r,Error::catalog,{NativePluginCatalogError::capacity});
        hexadecimal(r.source.owner_id,r.manifest_leaf);constexpr u32 suffix=sizeof(kNativePluginManifestSuffix)/sizeof(wchar_t)-1;
        for(u32 i=0;i<=suffix;++i)r.manifest_leaf[16+i]=kNativePluginManifestSuffix[i];r.manifest_characters=16+suffix;
        checked=native_plugin_pinned_file_open(r.destination_directory,{r.manifest_leaf,r.manifest_characters},&r.target_manifest);
        if(checked.ok()||checked.win32_error!=ERROR_FILE_NOT_FOUND)return fail(r,Error::conflict,checked);
    }
    if(cancelled(r))return fail(r,Error::cancelled);
    checked=native_plugin_pinned_dll_verify(r.source_directory,r.source,&r.source_dll);checkpoint(r);
    if(!checked.ok())return fail(r,Error::integrity,checked);
    checked=native_plugin_pinned_dll_verify(r.destination_directory,r.installed,&r.target_dll);checkpoint(r);
    const bool absent=!checked.ok()&&checked.win32_error==ERROR_FILE_NOT_FOUND;
    if(!checked.ok()&&!absent)return fail(r,Error::conflict,checked);
    if(retention_limit(r,absent)!=Error::none)return r.result.error;
    if(absent){
        // The missing-file check holds nothing after an ordinary failure,
        // but release before assigning a new file gate.
        checked=native_plugin_verified_file_release(&r.target_dll);
        if(!checked.ok())return fail(r,Error::release,checked);
        checked=native_plugin_pinned_file_open(r.destination_directory,{name,85},&r.target_dll,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,
            CREATE_NEW,FILE_FLAG_SEQUENTIAL_SCAN);
        r.created_dll=r.target_dll.file!=nullptr;checkpoint(r);if(!checked.ok())return fail(r,Error::open,checked);
        LARGE_INTEGER zero{};if(!SetFilePointerEx(r.source_dll.file,zero,nullptr,FILE_BEGIN))return fail(r,Error::copy,{NativePluginCatalogError::read,GetLastError()});
        u64 remaining=r.source_dll.bytes;
        while(remaining){if(cancelled(r))return fail(r,Error::cancelled);
            const DWORD wanted=static_cast<DWORD>(minimum<u64>(remaining,sizeof(r.copy_scratch)));DWORD read{},written{};
            if(!ReadFile(r.source_dll.file,r.copy_scratch,wanted,&read,nullptr)||read!=wanted||
                !WriteFile(r.target_dll.file,r.copy_scratch,wanted,&written,nullptr)||written!=wanted)return fail(r,Error::copy,{NativePluginCatalogError::read,GetLastError()});
            remaining-=wanted;
        }
        if(!FlushFileBuffers(r.target_dll.file))return fail(r,Error::flush,{NativePluginCatalogError::file,GetLastError()});
        r.target_dll.bytes=r.source_dll.bytes;
        checked=native_plugin_verified_file_hash(&r.target_dll,{r.installed.sha256,32});
        checkpoint(r);if(!checked.ok())return fail(r,Error::integrity,checked);
    }
    if(r.result.idempotent)return Error::none;
    if(!native_plugin_manifest_write(r.installed,{r.record,sizeof(r.record)},&r.record_bytes).ok())return fail(r,Error::manifest);
    if(r.manifest_characters+20>=128)return fail(r,Error::path);
    wchar_t temporary[128]{};for(u32 i=0;i<r.manifest_characters;++i)temporary[i]=r.manifest_leaf[i];
    const u32 base=r.manifest_characters;temporary[base]=L'.';temporary[base+1]=L'n';temporary[base+2]=L'e';temporary[base+3]=L'w';
    u64 nonce=hash64_mix(GetTickCount64(),GetCurrentProcessId());
    for(u32 attempt=0;attempt<16;++attempt){hexadecimal(nonce+attempt,temporary+base+4);temporary[base+20]=0;
        checked=native_plugin_pinned_file_open(r.destination_directory,{temporary,base+20},&r.staged_manifest,GENERIC_WRITE|DELETE,FILE_SHARE_READ,
            CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_WRITE_THROUGH);
        r.staged_file=r.staged_manifest.file!=nullptr;
        if(checked.ok())break;
        if(r.staged_manifest.file || (checked.win32_error!=ERROR_FILE_EXISTS&&checked.win32_error!=ERROR_ALREADY_EXISTS))return fail(r,Error::open,checked);
    }
    checkpoint(r);if(!checked.ok())return fail(r,Error::open,checked);
    DWORD written{};if(!WriteFile(r.staged_manifest.file,r.record,r.record_bytes,&written,nullptr)||written!=r.record_bytes)return fail(r,Error::copy,{NativePluginCatalogError::read,GetLastError()});
    if(!FlushFileBuffers(r.staged_manifest.file))return fail(r,Error::flush,{NativePluginCatalogError::file,GetLastError()});
    return Error::none;
}
Error publish(Runtime& r) noexcept {
    if(cancelled(r))return fail(r,Error::cancelled);
    if(r.result.idempotent){
        if(!manifest_still_expected(r))return fail(r,Error::conflict);
        r.result.committed=true;return Error::none;
    }
    if(r.result.update&&!manifest_still_expected(r))return fail(r,Error::conflict);
    if(r.retire_old){
        if(inventory(r)!=Error::none||other_reference(r,r.old))return fail(r,Error::conflict);
        wchar_t name[128]{};for(u32 i=0;i<r.old.dll_bytes;++i)name[i]=static_cast<wchar_t>(r.old.dll[i]);
        auto checked=native_plugin_pinned_file_open(r.destination_directory,{name,r.old.dll_bytes},&r.retired_dll,GENERIC_READ|DELETE,FILE_SHARE_READ);
        if(!checked.ok())return fail(r,Error::conflict,checked);
        if(!GetFileInformationByHandle(r.retired_dll.file,&r.retired_identity))return fail(r,Error::conflict,{NativePluginCatalogError::file,GetLastError()});
        // Authenticate the retained DELETE handle, not a pathname that
        // could name another version. No second cryptography implementation.
        checked=native_plugin_verified_file_hash(&r.retired_dll,{r.old.sha256,32});
        if(!checked.ok()||!final_path_equal(r.retired_dll))return fail(r,Error::integrity,checked);
    }
    if(cancelled(r))return fail(r,Error::cancelled);
    struct Rename {DWORD flags;HANDLE root;DWORD bytes;wchar_t name[1024];} rename{};
    static_assert(offsetof(Rename,name)==offsetof(FILE_RENAME_INFO,FileName));
    const u32 base=r.destination_directory.path_characters;
    const u32 characters=base+1+r.manifest_characters;if(characters>=1024)return fail(r,Error::path);
    for(u32 i=0;i<base;++i)rename.name[i]=r.destination_directory.path[i];rename.name[base]=L'\\';
    for(u32 i=0;i<r.manifest_characters;++i)rename.name[base+1+i]=r.manifest_leaf[i];rename.bytes=characters*2;
    rename.flags=r.result.update?3u:0u; // Documented REPLACE_IF_EXISTS | POSIX_SEMANTICS.
    // The old pin preserves bytes, not a pathname compare-and-swap. This
    // immediate identity/path/content recheck and cooperative lock define the
    // supported boundary; privileged same-user POSIX/ACL mutation is outside it.
    if(r.result.update&&!manifest_still_expected(r))return fail(r,Error::conflict);
    if(cancelled(r))return fail(r,Error::cancelled);
    BOOL renamed=FALSE;
    if(forced(8)){r.abandoned=true;return fail(r,Error::commit,{NativePluginCatalogError::file,ERROR_PROCESS_ABORTED});}
    if(r.result.update&&forced(1))SetLastError(ERROR_NOT_SUPPORTED);
    else if(forced(4))SetLastError(ERROR_ACCESS_DENIED);
    else renamed=SetFileInformationByHandle(r.staged_manifest.file,r.result.update?FileRenameInfoEx:FileRenameInfo,&rename,
        static_cast<DWORD>(offsetof(Rename,name))+rename.bytes);
    if(!renamed){const DWORD error=GetLastError();return fail(r,r.result.update&&(error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_PARAMETER||error==ERROR_INVALID_FUNCTION)?
        Error::unsupported:Error::commit,{NativePluginCatalogError::file,error});}
    r.staged_file=false;r.result.committed=true;
    u32 entry=r.catalog.count;
    for(u32 i=0;i<r.catalog.count;++i)if(r.catalog.entries[i].manifest.owner_id==r.installed.owner_id){entry=i;break;}
    if(entry==r.catalog.count)++r.catalog.count;
    auto& published=r.catalog.entries[entry];published={};published.manifest=r.installed;
    published.filename_characters=r.manifest_characters;
    for(u32 i=0;i<=r.manifest_characters;++i)published.filename[i]=r.manifest_leaf[i];
    // Visibility is already committed even when a later flush/cleanup fails.
    if(!FlushFileBuffers(r.staged_manifest.file))return fail(r,Error::flush,{NativePluginCatalogError::file,GetLastError()});
    return Error::none;
}
bool disposition(NativePluginVerifiedFile& file) noexcept {
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
    return !file.file||SetFileInformationByHandle(file.file,FileDispositionInfo,&disposition,sizeof(disposition))!=0;
}
bool keeps(const Runtime& r,const u8* sha256) noexcept {
    for(u32 i=0;i<r.keep_count&&i<2;++i)if(bytes_equal(r.keep[i],sha256,32))return true;
    return false;
}
// Deletes the owner's unreferenced managed DLLs outside r.keep whose content
// matches their name. Rescans manifests, so the staged write handle must be closed.
Error trim(Runtime& r) noexcept {
    if(inventory(r)!=Error::none)return r.result.error;
    auto& directory=r.destination_directory;const u32 base=directory.path_characters;
    if(base+3>=kNativePluginCatalogPathCapacity)return fail(r,Error::path);
    wchar_t pattern[kNativePluginCatalogPathCapacity]{};
    for(u32 i=0;i<base;++i)pattern[i]=directory.path[i];pattern[base]=L'\\';pattern[base+1]=L'*';
    WIN32_FIND_DATAW data{};directory.scan=FindFirstFileExW(pattern,FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(directory.scan==INVALID_HANDLE_VALUE){directory.scan=nullptr;return fail(r,Error::catalog,{NativePluginCatalogError::scan,GetLastError()});}
    u32 visited{},found{};wchar_t prefix[16];hexadecimal(r.installed.owner_id,prefix);Error error=Error::none;
    do{
        if(++visited>kNativePluginCatalogMaximumDirectoryEntries){error=Error::catalog;break;}
        u32 length{};while(length<MAX_PATH&&data.cFileName[length])++length;
        if(length!=85||data.cFileName[16]!=L'-'||data.cFileName[81]!=L'.'||ascii_lower(data.cFileName[82])!=L'd'||
            ascii_lower(data.cFileName[83])!=L'l'||ascii_lower(data.cFileName[84])!=L'l')continue;
        bool match=true;for(u32 i=0;i<16;++i)if(ascii_lower(data.cFileName[i])!=prefix[i])match=false;
        u8 digest[32]{};
        for(u32 i=0;match&&i<64;++i){const auto c=ascii_lower(data.cFileName[17+i]);
            const i32 nibble=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:-1;
            if(nibble<0)match=false;else digest[i/2]=static_cast<u8>(i%2?(digest[i/2]<<4)|nibble:nibble);}
        if(!match||keeps(r,digest))continue;
        if(found==8||(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){++r.result.kept;continue;}
        copy_bytes(r.candidates[found++],digest,32);
    }while(FindNextFileW(directory.scan,&data));
    if(error==Error::none&&GetLastError()!=ERROR_NO_MORE_FILES)error=Error::catalog;
    if(!FindClose(directory.scan))return fail(r,Error::release,{NativePluginCatalogError::release,GetLastError()});
    directory.scan=nullptr;
    if(error!=Error::none)return fail(r,error);
    for(u32 i=0;i<found;++i){
        NativePluginManifest candidate{};candidate.owner_id=r.installed.owner_id;copy_bytes(candidate.sha256,r.candidates[i],32);
        wchar_t name[128]{};managed_name(candidate,name);
        for(u32 j=0;j<85;++j)candidate.dll[j]=static_cast<char>(name[j]);candidate.dll_bytes=85;
        if(other_reference(r,candidate))continue; // an installed manifest (or an unreadable one) holds it
        // Authenticate the DELETE handle against the digest its name claims.
        const auto opened=native_plugin_pinned_file_open(directory,{name,85},&r.retired_dll,GENERIC_READ|DELETE,FILE_SHARE_READ);
        const bool verified=opened.ok()&&r.retired_dll.bytes<=kNativePluginDllMaximumBytes&&
            native_plugin_verified_file_hash(&r.retired_dll,{candidate.sha256,32}).ok()&&final_path_equal(r.retired_dll);
        if(verified&&disposition(r.retired_dll))++r.result.removed;else ++r.result.kept;
        const auto released=native_plugin_verified_file_release(&r.retired_dll);
        if(!released.ok())return fail(r,Error::release,released);
    }
    return Error::none;
}
// After a retain_previous commit: keep the installed and the replaced DLL.
Error trim_retained(Runtime& r) noexcept {
    if(!native_plugin_verified_file_release(&r.staged_manifest).ok())return fail(r,Error::release);
    copy_bytes(r.keep[0],r.installed.sha256,32);r.keep_count=1;
    if(managed(r.old)){copy_bytes(r.keep[1],r.old.sha256,32);r.keep_count=2;}
    return trim(r);
}
// After an uninstall commit: every owner-managed DLL goes.
Error trim_uninstalled(Runtime& r) noexcept {r.keep_count=0;return trim(r);}
Error stage_uninstall(Runtime& r) noexcept {
    if(!destination_valid(r))return fail(r,Error::path);
    auto checked=native_plugin_directory_pin({r.destination,r.destination_characters},&r.destination_directory);
    checkpoint(r);if(!checked.ok())return fail(r,Error::path,checked);
    r.result.owner_id=r.installed.owner_id;
    if(lock_owner(r,r.installed.owner_id)!=Error::none||inventory(r)!=Error::none)return r.result.error;
    u32 found=~u32{0};
    for(u32 i=0;i<r.catalog.count;++i)if(r.catalog.entries[i].manifest.owner_id==r.installed.owner_id){
        if(found!=~u32{0}||r.catalog.entries[i].status!=NativePluginCatalogError::none)return fail(r,Error::conflict);found=i;}
    if(found==~u32{0})return fail(r,Error::manifest,{NativePluginCatalogError::open,ERROR_FILE_NOT_FOUND});
    const auto& entry=r.catalog.entries[found];r.manifest_characters=entry.filename_characters;
    for(u32 i=0;i<=r.manifest_characters;++i)r.manifest_leaf[i]=entry.filename[i];
    // A delete-sharing pin keeps the bytes the user confirmed; commit rechecks them.
    checked=native_plugin_pinned_manifest_read(r.destination_directory,{r.manifest_leaf,r.manifest_characters},&r.old,&r.target_manifest,
        FILE_SHARE_READ|FILE_SHARE_DELETE);
    checkpoint(r);
    if(!checked.ok()||!same_manifest(r.old,entry.manifest)||!GetFileInformationByHandle(r.target_manifest.file,&r.old_identity))
        return fail(r,Error::conflict,checked);
    return Error::none;
}
Error publish_uninstall(Runtime& r) noexcept {
    if(cancelled(r))return fail(r,Error::cancelled);
    if(!manifest_still_expected(r))return fail(r,Error::conflict);
    auto checked=native_plugin_pinned_file_open(r.destination_directory,{r.manifest_leaf,r.manifest_characters},&r.staged_manifest,
        GENERIC_READ|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE);
    if(!checked.ok())return fail(r,Error::conflict,checked);
    BY_HANDLE_FILE_INFORMATION identity{};
    if(!GetFileInformationByHandle(r.staged_manifest.file,&identity)||!identity_equal(identity,r.old_identity)||!final_path_equal(r.staged_manifest))
        return fail(r,Error::conflict);
    if(cancelled(r))return fail(r,Error::cancelled);
    if(forced(4)){SetLastError(ERROR_ACCESS_DENIED);return fail(r,Error::commit,{NativePluginCatalogError::file,ERROR_ACCESS_DENIED});}
    if(!disposition(r.staged_manifest))return fail(r,Error::commit,{NativePluginCatalogError::file,GetLastError()});
    // Closing both pins removes the name; the snapshot drops the owner.
    r.result.committed=true;
    for(u32 i=0;i<r.catalog.count;++i)if(r.catalog.entries[i].manifest.owner_id==r.installed.owner_id){
        for(u32 j=i+1;j<r.catalog.count;++j)r.catalog.entries[j-1]=r.catalog.entries[j];--r.catalog.count;break;}
    checked=native_plugin_verified_file_release(&r.staged_manifest);
    if(checked.ok())checked=native_plugin_verified_file_release(&r.target_manifest);
    if(!checked.ok())return fail(r,Error::release,checked);
    return Error::none;
}
bool cleanup(Runtime& r) noexcept {
    if(forced(2)){r.result.cleanup_pending=true;return false;}
    // A simulated hard crash (fixture 8) leaves its files for recovery.
    if(r.abandoned){r.staged_file=false;r.created_dll=false;}
    if(r.result.committed&&r.after_commit&&!r.after_done){
        if(r.after_commit(r)!=Error::none){r.result.cleanup_pending=true;return false;}
        r.after_done=true;
    }
    if(r.result.committed&&r.retire_old&&!r.old_deleted){
        // The write handle must close before a fresh manifest scan
        // can open its new name with the existing read-only sharing contract.
        if(!native_plugin_verified_file_release(&r.staged_manifest).ok()||inventory(r)!=Error::none||other_reference(r,r.old)){
            r.result.cleanup_pending=true;return false;
        }
        BY_HANDLE_FILE_INFORMATION identity{};
        if(!GetFileInformationByHandle(r.retired_dll.file,&identity)||!identity_equal(identity,r.retired_identity)||!final_path_equal(r.retired_dll)||
            !disposition(r.retired_dll)){r.result.cleanup_pending=true;return false;}r.old_deleted=true;
    }
    if(r.staged_file){if(!disposition(r.staged_manifest)){r.result.cleanup_pending=true;return false;}r.staged_file=false;}
    if(r.created_dll&&!r.result.committed){if(!disposition(r.target_dll)){r.result.cleanup_pending=true;return false;}r.created_dll=false;}
    NativePluginVerifiedFile* files[]{&r.validation,&r.retired_dll,&r.staged_manifest,&r.target_dll,&r.target_manifest,&r.source_dll,&r.source_manifest};
    for(auto* file:files)if(!native_plugin_verified_file_release(file).ok()){r.result.cleanup_pending=true;return false;}
    if(!native_plugin_catalog_release(&r.catalog).ok()){r.result.cleanup_pending=true;return false;}
    // Keep the cooperative lock and both parent banks until every file owner
    // has finished. DELETE_ON_CLOSE removes only our exclusive CREATE_NEW lock.
    NativePluginVerifiedFile* directories[]{&r.lock,&r.destination_directory,&r.source_directory};
    for(auto* file:directories)if(!native_plugin_verified_file_release(file).ok()){r.result.cleanup_pending=true;return false;}
    r.result.cleanup_pending=false;return true;
}
// One operation: prepare, publish the prepared snapshot, wait for the UI's
// commit or cancel, commit, then clean up.
DWORD run(Runtime& r,Error (*prepare)(Runtime&),Error (*commit)(Runtime&)) noexcept {
    if(!r.cleanup_only){
        if(prepare(r)==Error::none){
            if(cancelled(r))fail(r,Error::cancelled);
            else {
                checkpoint(r);r.resources.published=true;
                if(InterlockedCompareExchange(&r.owner->phase,phase_value(Phase::prepared),phase_value(Phase::staging))!=phase_value(Phase::staging)){
                    fail(r,Error::cancelled);
                }else{
                SetEvent(r.owner->notification);
                const DWORD waited=WaitForSingleObject(r.owner->command,INFINITE);
                // Only the UI thread may release a readable prepared snapshot.
                // An impossible wait failure/spurious external command instead
                // sets an atomic failure marker without changing that
                // snapshot; a cleanup retry acquires the exited worker.
                if(waited!=WAIT_OBJECT_0||phase(*r.owner)==Phase::prepared){
                    InterlockedExchange(&r.wait_failed,1);InterlockedExchange(&r.owner->phase,phase_value(Phase::complete));
                    SetEvent(r.owner->notification);return 0;
                }
                if(cancelled(r)||!InterlockedCompareExchange(&r.owner->commit_requested,0,0))fail(r,Error::cancelled);
                else commit(r);
                }
            }
        }
    }
    cleanup(r);checkpoint(r);r.resources.published=true;
    InterlockedExchange(&r.owner->phase,phase_value(Phase::complete));SetEvent(r.owner->notification);return 0;
}
DWORD WINAPI worker(void* pointer) noexcept {return run(*static_cast<Runtime*>(pointer),stage,publish);}
DWORD WINAPI uninstall_worker(void* pointer) noexcept {return run(*static_cast<Runtime*>(pointer),stage_uninstall,publish_uninstall);}
bool input(ConstSlice<wchar_t> value) noexcept {
    if(!value.data||value.count<3||value.count>=kNativePluginCatalogPathCapacity||
        !byte_range_valid({reinterpret_cast<const u8*>(value.data),value.count*2}))return false;
    for(u32 i=0;i<value.count;++i)if(!value[i])return false;return true;
}
bool readable(const NativePluginBundleInstall* owner) noexcept {
    return owner&&owner->runtime&&(phase(*owner)==Phase::prepared||phase(*owner)==Phase::complete);
}
Runtime* allocate_runtime() noexcept {
    SYSTEM_INFO system{};GetSystemInfo(&system);if(!system.dwPageSize)return nullptr;
    const usize bytes=(sizeof(Runtime)+system.dwPageSize-1)/system.dwPageSize*system.dwPageSize;
    auto* r=static_cast<Runtime*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(r)r->resources={bytes,sizeof(Runtime),sizeof(r->copy_scratch),3,3,false};
    return r;
}
struct Launch {
    LPTHREAD_START_ROUTINE thread;
    Error (*after_commit)(Runtime&);
    u64 uninstall_owner;
    bool retain_previous;
};
// manifest_path is empty for an uninstall.
Error launch(NativePluginBundleInstall* owner,ConstSlice<wchar_t> manifest_path,ConstSlice<wchar_t> destination,const Launch& with) noexcept {
    if(!owner||(with.uninstall_owner?manifest_path.count!=0:!input(manifest_path))||!input(destination))return Error::invalid_input;
    if(owner->runtime||owner->thread||owner->notification||owner->command)return Error::busy;
    const ConstSlice<u8> own{reinterpret_cast<const u8*>(owner),sizeof(*owner)};
    if(!byte_range_valid(own)||(manifest_path.count&&byte_ranges_overlap(own,{reinterpret_cast<const u8*>(manifest_path.data),manifest_path.count*2}))||
        byte_ranges_overlap(own,{reinterpret_cast<const u8*>(destination.data),destination.count*2}))return Error::invalid_input;
    auto* r=allocate_runtime();if(!r)return Error::memory;
    r->owner=owner;r->after_commit=with.after_commit;r->retain_previous=with.retain_previous;
    r->installed.owner_id=with.uninstall_owner;r->result.uninstall=with.uninstall_owner!=0;
    r->input_characters=manifest_path.count;r->destination_characters=destination.count;
    for(u32 i=0;i<manifest_path.count;++i)r->input[i]=manifest_path[i];for(u32 i=0;i<destination.count;++i)r->destination[i]=destination[i];
    owner->runtime=r;owner->notification=CreateEventW(nullptr,TRUE,FALSE,nullptr);owner->command=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!owner->notification||!owner->command){r->result.error=Error::memory;InterlockedExchange(&owner->phase,phase_value(Phase::complete));
        native_plugin_bundle_install_release(owner);return Error::memory;}
    InterlockedExchange(&owner->phase,phase_value(Phase::staging));owner->thread=CreateThread(nullptr,256*1024,with.thread,r,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!owner->thread){r->result.error=Error::thread;InterlockedExchange(&owner->phase,phase_value(Phase::complete));native_plugin_bundle_install_release(owner);return Error::thread;}
    return Error::none;
}
// Recovery: the installer's staged "<owner16>.maicord-plugin.json.new<16 hex>" names.
bool staged_manifest_name(const wchar_t* name,u64 owner_id) noexcept {
    wchar_t prefix[16];hexadecimal(owner_id,prefix);
    constexpr wchar_t suffix[]=L".maicord-plugin.json.new";constexpr u32 suffix_characters=sizeof(suffix)/sizeof(wchar_t)-1;
    u32 length{};while(length<MAX_PATH&&name[length])++length;
    if(length!=16+suffix_characters+16)return false;
    for(u32 i=0;i<16;++i)if(ascii_lower(name[i])!=prefix[i])return false;
    for(u32 i=0;i<suffix_characters;++i)if(ascii_lower(name[16+i])!=suffix[i])return false;
    for(u32 i=16+suffix_characters;i<length;++i){const auto c=ascii_lower(name[i]);if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;}
    return true;
}
Error remove_staged_manifests(Runtime& r,NativePluginBundleRecoveryResult* out) noexcept {
    auto& directory=r.destination_directory;const u32 base=directory.path_characters;
    if(base+3>=kNativePluginCatalogPathCapacity)return fail(r,Error::path);
    wchar_t pattern[kNativePluginCatalogPathCapacity]{};
    for(u32 i=0;i<base;++i)pattern[i]=directory.path[i];pattern[base]=L'\\';pattern[base+1]=L'*';
    constexpr u32 kNameCharacters=56;wchar_t names[8][kNameCharacters+1]{};u32 found{},visited{};
    WIN32_FIND_DATAW data{};directory.scan=FindFirstFileExW(pattern,FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(directory.scan==INVALID_HANDLE_VALUE){directory.scan=nullptr;return fail(r,Error::catalog,{NativePluginCatalogError::scan,GetLastError()});}
    Error error=Error::none;
    do{
        if(++visited>kNativePluginCatalogMaximumDirectoryEntries){error=Error::catalog;break;}
        if(!staged_manifest_name(data.cFileName,r.installed.owner_id))continue;
        if(found==8||(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||data.nFileSizeHigh||
            data.nFileSizeLow>kNativePluginManifestMaximumBytes){++out->kept;continue;}
        for(u32 i=0;i<=kNameCharacters;++i)names[found][i]=data.cFileName[i];++found;
    }while(FindNextFileW(directory.scan,&data));
    if(error==Error::none&&GetLastError()!=ERROR_NO_MORE_FILES)error=Error::catalog;
    if(!FindClose(directory.scan))return fail(r,Error::release,{NativePluginCatalogError::release,GetLastError()});
    directory.scan=nullptr;
    if(error!=Error::none)return fail(r,error);
    for(u32 i=0;i<found;++i){
        const auto opened=native_plugin_pinned_file_open(directory,{names[i],kNameCharacters},&r.staged_manifest,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        if(opened.ok()&&final_path_equal(r.staged_manifest)&&disposition(r.staged_manifest))++out->removed_manifests;else ++out->kept;
        const auto released=native_plugin_verified_file_release(&r.staged_manifest);
        if(!released.ok())return fail(r,Error::release,released);
    }
    return Error::none;
}
} // namespace
NativePluginBundleInstallError native_plugin_bundle_install_start(NativePluginBundleInstall* owner,
    ConstSlice<wchar_t> manifest_path,ConstSlice<wchar_t> destination) noexcept {
    return launch(owner,manifest_path,destination,{worker,nullptr,0,false});
}
NativePluginBundleInstallError native_plugin_bundle_install_start(NativePluginBundleInstall* owner,
    ConstSlice<wchar_t> manifest_path,ConstSlice<wchar_t> destination,NativePluginBundleInstallOptions options) noexcept {
    return launch(owner,manifest_path,destination,{worker,options.retain_previous?trim_retained:nullptr,0,options.retain_previous});
}
NativePluginBundleInstallError native_plugin_bundle_uninstall_start(NativePluginBundleInstall* owner,u64 owner_id,
    ConstSlice<wchar_t> destination) noexcept {
    if(owner_id<=1)return Error::invalid_input;
    return launch(owner,{},destination,{uninstall_worker,trim_uninstalled,owner_id,false});
}
NativePluginBundleRecoveryResult native_plugin_bundle_recover(u64 owner_id,ConstSlice<wchar_t> destination,
    const NativePluginBundleKeep& keep) noexcept {
    NativePluginBundleRecoveryResult out{};
    if(owner_id<=1||!input(destination)||keep.count>2){out.error=Error::invalid_input;return out;}
    auto* r=allocate_runtime();if(!r){out.error=Error::memory;return out;}
    r->installed.owner_id=owner_id;r->keep_count=keep.count;copy_bytes(r->keep,keep.sha256,sizeof(r->keep));
    r->destination_characters=destination.count;for(u32 i=0;i<destination.count;++i)r->destination[i]=destination[i];
    Error error=destination_valid(*r)?Error::none:fail(*r,Error::path);
    if(error==Error::none){
        const auto pinned=native_plugin_directory_pin({r->destination,r->destination_characters},&r->destination_directory);
        if(!pinned.ok())error=fail(*r,Error::path,pinned);
    }
    // The owner lock excludes installers; the caller's journal names the owner.
    if(error==Error::none)error=lock_owner(*r,owner_id);
    if(error==Error::none)error=remove_staged_manifests(*r,&out);
    if(error==Error::none)error=trim(*r);
    out.removed_dlls=r->result.removed;out.kept+=r->result.kept;
    out.error=error;out.filesystem=r->result.filesystem;
    NativePluginVerifiedFile* files[]{&r->validation,&r->retired_dll,&r->staged_manifest};
    bool released=true;
    for(auto* file:files)released=native_plugin_verified_file_release(file).ok()&&released;
    released=native_plugin_catalog_release(&r->catalog).ok()&&released;
    NativePluginVerifiedFile* directories[]{&r->lock,&r->destination_directory};
    for(auto* file:directories)released=native_plugin_verified_file_release(file).ok()&&released;
    // A failed release keeps its handles (and the runtime) rather than freeing live state.
    if(!released){if(out.error==Error::none)out.error=Error::release;return out;}
    VirtualFree(r,0,MEM_RELEASE);
    return out;
}
NativePluginBundleInstallResult native_plugin_bundle_install_status(const NativePluginBundleInstall* owner) noexcept {
    NativePluginBundleInstallResult result{};if(!owner)return {Phase::empty,Error::invalid_input};result.phase=phase(*owner);
    if(readable(owner)){const auto& r=*static_cast<const Runtime*>(owner->runtime);result=r.result;result.phase=phase(*owner);
        if(InterlockedCompareExchange(const_cast<volatile LONG*>(&r.wait_failed),0,0)){result.error=Error::wait;result.cleanup_pending=true;}}
    return result;
}
bool native_plugin_bundle_install_catalog(const NativePluginBundleInstall* owner,NativePluginCatalog* catalog,
    Slice<NativePluginCatalogEntry> entries) noexcept {
    if(!readable(owner)||!catalog||catalog->cleanup||!entries.data||entries.count!=32)return false;
    const auto& r=*static_cast<const Runtime*>(owner->runtime);
    if(!r.result.committed||!r.catalog_ready||r.catalog.count>entries.count)return false;
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(catalog),sizeof(*catalog)},
        {reinterpret_cast<const u8*>(entries.data),entries.count*static_cast<u32>(sizeof(*entries.data))},
        {reinterpret_cast<const u8*>(owner),sizeof(*owner)},{reinterpret_cast<const u8*>(&r),sizeof(r)}};
    for(u32 i=0;i<4;++i){if(!byte_range_valid(ranges[i]))return false;
        for(u32 j=0;j<i;++j)if(byte_ranges_overlap(ranges[i],ranges[j]))return false;}
    if(!native_plugin_catalog_initialize(catalog,{r.destination,r.destination_characters},entries).ok())return false;
    catalog->count=r.catalog.count;
    for(u32 i=0;i<catalog->count;++i)entries[i]=r.catalog.entries[i];
    // The catalogue scan orders names; append must preserve that stable window.
    for(u32 i=1;i<catalog->count;++i){auto value=entries[i];u32 j=i;
        while(j){const auto& previous=entries[j-1];u32 k{};
            while(k<value.filename_characters&&k<previous.filename_characters&&ascii_lower(value.filename[k])==ascii_lower(previous.filename[k]))++k;
            const bool before=k<value.filename_characters&&(k==previous.filename_characters||ascii_lower(value.filename[k])<ascii_lower(previous.filename[k]));
            if(!before)break;entries[j]=previous;--j;}entries[j]=value;}
    return true;
}
NativePluginBundleInstallError native_plugin_bundle_install_commit(NativePluginBundleInstall* owner) noexcept {
    if(!owner||!owner->runtime)return Error::invalid_input;
    if(InterlockedCompareExchange(&owner->cancelled,0,0)||InterlockedCompareExchange(&owner->phase,phase_value(Phase::publishing),phase_value(Phase::prepared))!=phase_value(Phase::prepared))return Error::busy;
    InterlockedExchange(&owner->commit_requested,1);ResetEvent(owner->notification);
    if(!SetEvent(owner->command)){InterlockedExchange(&owner->cancelled,1);return Error::wait;}return Error::none;
}
void native_plugin_bundle_install_cancel(NativePluginBundleInstall* owner) noexcept {
    if(!owner||!owner->runtime)return;
    if(owner->notification)ResetEvent(owner->notification);
    InterlockedExchange(&owner->cancelled,1);
    InterlockedCompareExchange(&owner->phase,phase_value(Phase::cleanup),phase_value(Phase::staging));
    InterlockedCompareExchange(&owner->phase,phase_value(Phase::cleanup),phase_value(Phase::prepared));
    if(owner->command)SetEvent(owner->command);
    if(phase(*owner)==Phase::complete&&owner->notification)SetEvent(owner->notification);
}
void native_plugin_bundle_install_acknowledge(NativePluginBundleInstall* owner) noexcept {if(owner&&owner->notification)ResetEvent(owner->notification);}
NativePluginBundleInstallError native_plugin_bundle_install_retry_cleanup(NativePluginBundleInstall* owner) noexcept {
    if(!readable(owner)||phase(*owner)!=Phase::complete)return Error::busy;
    auto& r=*static_cast<Runtime*>(owner->runtime);
    const bool wait_failed=InterlockedCompareExchange(&r.wait_failed,0,0)!=0;
    if(!r.result.cleanup_pending&&!wait_failed)return Error::invalid_input;
    if(owner->thread){if(WaitForSingleObject(owner->thread,0)!=WAIT_OBJECT_0)return Error::busy;
        if(!CloseHandle(owner->thread))return Error::release;owner->thread=nullptr;}
    ResetEvent(owner->notification);InterlockedExchange(&owner->phase,phase_value(Phase::cleanup));
    if(wait_failed){r.result.error=Error::wait;r.result.cleanup_pending=true;InterlockedExchange(&r.wait_failed,0);}
    r.cleanup_only=true;
    owner->thread=CreateThread(nullptr,256*1024,worker,&r,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!owner->thread){InterlockedExchange(&owner->phase,phase_value(Phase::complete));return Error::thread;}return Error::none;
}
bool native_plugin_bundle_install_release(NativePluginBundleInstall* owner,u32 timeout_ms) noexcept {
    if(!owner)return false;if(!owner->runtime)return !owner->thread&&!owner->notification&&!owner->command;
    if(owner->thread&&WaitForSingleObject(owner->thread,timeout_ms)!=WAIT_OBJECT_0)return false;
    auto& r=*static_cast<Runtime*>(owner->runtime);if(r.result.cleanup_pending||InterlockedCompareExchange(&r.wait_failed,0,0))return false;
    if(owner->thread){if(!CloseHandle(owner->thread))return false;owner->thread=nullptr;}
    if(owner->command){if(!CloseHandle(owner->command))return false;owner->command=nullptr;}
    if(owner->notification){if(!CloseHandle(owner->notification))return false;owner->notification=nullptr;}
    if(!VirtualFree(owner->runtime,0,MEM_RELEASE))return false;*owner={};return true;
}
NativePluginBundleInstallResources native_plugin_bundle_install_resources(const NativePluginBundleInstall* owner) noexcept {
    if(!owner||!owner->runtime)return {};
    // Worker snapshots are read only after an acquire of a stable
    // phase. The UI thread keeps the runtime alive; no live counters are read.
    if(!readable(owner))return {0,sizeof(Runtime),8192,0,0,false};
    return static_cast<const Runtime*>(owner->runtime)->resources;
}
#if defined(MAICORD_PLUGIN_BUNDLE_INSTALL_FIXTURE)
void native_plugin_bundle_install_fixture_failures(u32 mask) noexcept {InterlockedExchange(&failure_mask,static_cast<LONG>(mask&15));}
#endif
} // namespace maicord

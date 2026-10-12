#include "maicord/app/plugin_bundle_install.hpp"
#include "maicord/core/utf.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace bundle_install_test {
using Owner=NativePluginBundleInstall;
using Phase=NativePluginBundleInstallPhase;
using Error=NativePluginBundleInstallError;
void measurement(const char* key,u64 value) noexcept {
    test_write_metric(key, value);
}
struct Fixture {
    wchar_t root[1024]{},source[1024]{},destination[1024]{},parent[1024]{},input[3][1024]{};
    wchar_t created[16][1024]{};u32 created_count{};
    NativePluginManifest manifest[3]{};
    Owner owner{},second{};bool directories{};
};
u32 length(const wchar_t* p) noexcept {u32 n{};while(n<1023&&p[n])++n;return n;}
bool join(wchar_t* out,const wchar_t* directory,const wchar_t* leaf) noexcept {
    u32 n=length(directory);if(n>=1023)return false;
    for(u32 i=0;i<n;++i)out[i]=directory[i];if(n&&out[n-1]!=L'\\')out[n++]=L'\\';
    for(u32 i=0;leaf[i];++i){if(n>=1023)return false;out[n++]=leaf[i];}out[n]=0;return true;
}
bool write(Fixture& f,const wchar_t* path,const void* bytes,u32 count) noexcept {
    if(f.created_count==16)return false;
    const HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const u32 n=length(path);for(u32 i=0;i<=n;++i)f.created[f.created_count][i]=path[i];++f.created_count;
    DWORD written{};const bool okay=WriteFile(file,bytes,count,&written,nullptr)&&written==count;
    return CloseHandle(file)!=0&&okay;
}
void text(char* out,u32& count,Utf8View value) noexcept {count=value.bytes;for(u32 i=0;i<count;++i)out[i]=value.data[i];}
void managed_leaf(const NativePluginManifest& m,wchar_t* leaf) noexcept {
    constexpr wchar_t hex[]=L"0123456789abcdef";for(u32 i=0;i<16;++i)leaf[i]=hex[(m.owner_id>>((15-i)*4))&15];leaf[16]=L'-';
    for(u32 i=0;i<32;++i){leaf[17+i*2]=hex[m.sha256[i]>>4];leaf[18+i*2]=hex[m.sha256[i]&15];}
    leaf[81]=L'.';leaf[82]=L'd';leaf[83]=L'l';leaf[84]=L'l';leaf[85]=0;
}
bool hash(const void* data,u32 bytes,u8* digest) noexcept {
    BCRYPT_HASH_HANDLE handle{};u8 object[4096];DWORD needed{},returned{};
    bool okay=BCryptGetProperty(BCRYPT_SHA256_ALG_HANDLE,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&needed),sizeof(needed),&returned,0)>=0&&
        returned==sizeof(needed)&&needed&&needed<=sizeof(object)&&BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&handle,object,needed,nullptr,0,0)>=0;
    if(okay)okay=BCryptHashData(handle,const_cast<PUCHAR>(static_cast<const u8*>(data)),bytes,0)>=0&&BCryptFinishHash(handle,digest,32,0)>=0;
    if(handle&&BCryptDestroyHash(handle)<0)okay=false;return okay;
}
bool setup(Fixture& f) noexcept {
    wchar_t temp[1024]{},leaf[64]{};const DWORD count=GetTempPathW(1024,temp);if(!count||count>=1024)return false;
    constexpr wchar_t prefix[]=L"MaiCordBundleFixture-";u32 at{};for(;at<sizeof(prefix)/2-1;++at)leaf[at]=prefix[at];
    const u64 nonce=(u64{GetCurrentProcessId()}<<32)^GetTickCount64();constexpr wchar_t hex[]=L"0123456789abcdef";
    for(u32 i=0;i<16;++i)leaf[at++]=hex[(nonce>>((15-i)*4))&15];
    if(!join(f.root,temp,leaf)||!CreateDirectoryW(f.root,nullptr))return false;
    if(!join(f.source,f.root,L"source")||!CreateDirectoryW(f.source,nullptr)||!join(f.parent,f.root,L"MaiCord")||
        !join(f.destination,f.parent,L"plugins"))return false;f.directories=true;
    for(u32 i=0;i<3;++i){auto& m=f.manifest[i];m.owner_id=0x123456789abcdef0ull;m.abi_version=5;
        text(m.name,m.name_bytes,utf8_literal("Fixture \"plugin\""));text(m.description,m.description_bytes,utf8_literal("Local \\ trusted metadata"));
        m.version[0]=static_cast<char>('1'+i);m.version_bytes=1;
        constexpr char dll[]= "input1.dll";for(u32 j=0;j<sizeof(dll)-1;++j)m.dll[j]=dll[j];m.dll[5]=static_cast<char>('1'+i);m.dll_bytes=sizeof(dll)-1;
        u8 bytes[64];for(u32 j=0;j<64;++j)bytes[j]=static_cast<u8>(j+i*17);
        if(!hash(bytes,64,m.sha256))return false;
        wchar_t dll_leaf[32]{},path[1024]{};for(u32 j=0;j<m.dll_bytes;++j)dll_leaf[j]=static_cast<wchar_t>(m.dll[j]);
        if(!join(path,f.source,dll_leaf)||!write(f,path,bytes,64))return false;
        wchar_t manifest_leaf[]=L"bundle1.maicord-plugin.json";manifest_leaf[6]=static_cast<wchar_t>(L'1'+i);
        char record[4096];u32 used{};
        if(!join(f.input[i],f.source,manifest_leaf)||!native_plugin_manifest_write(m,{record,4096},&used).ok()||!write(f,f.input[i],record,used))return false;
    }
    return true;
}
bool start(Fixture& f,Owner& owner,u32 source) noexcept {
    return native_plugin_bundle_install_start(&owner,{f.input[source],length(f.input[source])},{f.destination,length(f.destination)})==Error::none;
}
bool await(Owner& owner,Phase wanted) noexcept {
    const u64 limit=GetTickCount64()+2000;
    for(u32 attempt=0;attempt<8;++attempt){
        if(native_plugin_bundle_install_status(&owner).phase==wanted)return true;
        const u64 now=GetTickCount64();if(now>=limit)return false;
        if(WaitForSingleObject(owner.notification,static_cast<DWORD>(limit-now))!=WAIT_OBJECT_0)return false;
        // Reset then recheck: a phase published before/after ResetEvent cannot
        // lose a completion. These tests create no Shell dialog or DLL load.
        native_plugin_bundle_install_acknowledge(&owner);
    }
    return native_plugin_bundle_install_status(&owner).phase==wanted;
}
bool release(Owner& owner) noexcept {return native_plugin_bundle_install_release(&owner,2000);}
bool installed(Fixture& f,u32 source) noexcept {
    NativePluginCatalogEntry entries[32]{};NativePluginCatalog catalog{};
    bool okay=native_plugin_catalog_initialize(&catalog,{f.destination,length(f.destination)},{entries,32}).ok()&&native_plugin_catalog_refresh(&catalog).ok();
    okay=okay&&catalog.count==1&&entries[0].status==NativePluginCatalogError::none&&bytes_equal(entries[0].manifest.sha256,f.manifest[source].sha256,32);
    wchar_t expected[128]{};managed_leaf(f.manifest[source],expected);
    okay=okay&&entries[0].manifest.dll_bytes==85;
    if(okay)for(u32 i=0;i<85;++i)if(static_cast<wchar_t>(entries[0].manifest.dll[i])!=expected[i])okay=false;
    return native_plugin_catalog_release(&catalog).ok()&&okay;
}
u32 managed_count(Fixture& f,u64* bytes) noexcept {
    wchar_t pattern[1024]{};if(!join(pattern,f.destination,L"*.dll"))return ~u32{0};WIN32_FIND_DATAW found{};
    const HANDLE scan=FindFirstFileW(pattern,&found);if(scan==INVALID_HANDLE_VALUE){*bytes=0;return GetLastError()==ERROR_FILE_NOT_FOUND?0:~u32{0};}
    u32 count{};*bytes=0;
    do{if(++count>16)break;*bytes+=(u64{found.nFileSizeHigh}<<32)|found.nFileSizeLow;}while(FindNextFileW(scan,&found));
    if(!FindClose(scan))return ~u32{0};return count;
}
void cleanup(Fixture& f) noexcept {
    native_plugin_bundle_install_fixture_failures(0);
    Owner* owners[]{&f.owner,&f.second};for(auto* owner:owners){
        native_plugin_bundle_install_cancel(owner);
        if(owner->runtime){await(*owner,Phase::complete);const auto result=native_plugin_bundle_install_status(owner);
            if(result.cleanup_pending&&native_plugin_bundle_install_retry_cleanup(owner)==Error::none)await(*owner,Phase::complete);
            if(!release(*owner))return;}
    }
    // Only direct fixture-created children of our exclusive new directory.
    wchar_t pattern[1024]{};WIN32_FIND_DATAW found{};
    if(join(pattern,f.destination,L"*")){const HANDLE scan=FindFirstFileW(pattern,&found);
        if(scan!=INVALID_HANDLE_VALUE){u32 count{};do{if(++count>32)break;
            if(!(found.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){wchar_t path[1024]{};if(join(path,f.destination,found.cFileName))DeleteFileW(path);}
        }while(FindNextFileW(scan,&found));FindClose(scan);}}
    for(u32 i=0;i<f.created_count;++i)DeleteFileW(f.created[i]);
    RemoveDirectoryW(f.destination);RemoveDirectoryW(f.parent);RemoveDirectoryW(f.source);RemoveDirectoryW(f.root);
}
}
void test_plugin_bundle_install(void (*check)(bool,const char*) noexcept) noexcept {
    using namespace bundle_install_test;
    static Fixture f{};struct Restore{Fixture& f;~Restore() noexcept{cleanup(f);}} restore{f};
    const bool initialized=setup(f);check(initialized,"bundle install: exclusive offline fixture with three bounded DLL byte variants and escaped metadata");if(!initialized)return;
    char record[4096]{};u32 count=77;NativePluginManifest roundtrip{};
    check(native_plugin_manifest_write(f.manifest[0],{record,4096},&count).ok()&&native_plugin_manifest_parse({record,count},&roundtrip).ok()&&
        bytes_equal(&roundtrip,&f.manifest[0],sizeof(roundtrip)),"bundle manifest: canonical serialization roundtrips full owner, digest and escaped metadata");
    record[0]='!';count=77;
    check(native_plugin_manifest_write(f.manifest[0],{record,1},&count).error==NativePluginManifestError::capacity&&record[0]=='!'&&count==77&&
        native_plugin_manifest_write(f.manifest[0],{reinterpret_cast<char*>(&f.manifest[0]),sizeof(f.manifest[0])},&count).error==NativePluginManifestError::invalid_input,
        "bundle manifest: capacity and alias refusals preserve outputs atomically");
    check(start(f,f.owner,0)&&await(f.owner,Phase::prepared),"bundle install: worker stages off caller with final manifest absent");
    if(native_plugin_bundle_install_status(&f.owner).phase!=Phase::prepared)return;
    NativePluginManifest prepared{};const auto resources=native_plugin_bundle_install_resources(&f.owner);
    measurement("plugin_install_owner_bytes=",resources.owned_bytes);
    measurement("plugin_install_owner_committed_bytes=",resources.committed_bytes);
    measurement("plugin_install_copy_scratch_bytes=",resources.copy_scratch_bytes);
    measurement("plugin_install_prepared_owned_handle_tokens=",resources.attached_handles);
    measurement("plugin_install_peak_checkpoint_handle_tokens=",resources.peak_checkpoint_handles);
    check(native_plugin_bundle_install_manifest(&f.owner,&prepared)&&prepared.dll_bytes==85&&resources.published&&resources.copy_scratch_bytes==8192&&
        resources.owned_bytes<=96*1024&&resources.committed_bytes>=resources.owned_bytes,
        "bundle install: exact managed hash leaf, bounded VM metadata and streaming scratch are published");
    check(start(f,f.second,0)&&await(f.second,Phase::complete)&&native_plugin_bundle_install_status(&f.second).error==Error::busy&&release(f.second),
        "bundle install: same-owner concurrent worker cannot pass exclusive cooperative lock");
    check(native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&native_plugin_bundle_install_status(&f.owner).committed&&
        release(f.owner)&&installed(f,0),"bundle install: manifest-last fresh publication creates verified disabled inventory without loading code");
    u64 disk{};check(managed_count(f,&disk)==1&&disk==64,"bundle install: fresh owner retains one 64-byte managed fixture DLL");
    check(start(f,f.owner,0)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_status(&f.owner).idempotent&&
        native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&release(f.owner)&&managed_count(f,&disk)==1,
        "bundle install: exact already-installed identity is idempotent with no extra managed version");
    check(start(f,f.owner,1)&&await(f.owner,Phase::prepared),"bundle update: changed code stages before UI unload authorization");
    native_plugin_bundle_install_cancel(&f.owner);
    const auto cancelled=native_plugin_bundle_install_status(&f.owner);
    check(cancelled.phase!=Phase::prepared&&await(f.owner,Phase::complete)&&native_plugin_bundle_install_status(&f.owner).error==Error::cancelled&&
        release(f.owner)&&installed(f,0)&&managed_count(f,&disk)==1,"bundle update: prepared cancellation retires snapshot and leaves old manifest/code authoritative");
    native_plugin_bundle_install_fixture_failures(1);
    check(start(f,f.owner,1)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).error==Error::unsupported&&!native_plugin_bundle_install_status(&f.owner).committed&&release(f.owner)&&installed(f,0)&&managed_count(f,&disk)==1,
        "bundle update: unsupported pinned POSIX replacement fails closed with no legacy delete/overwrite fallback");
    native_plugin_bundle_install_fixture_failures(4);
    check(start(f,f.owner,1)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).error==Error::commit&&!native_plugin_bundle_install_status(&f.owner).committed&&release(f.owner)&&installed(f,0)&&managed_count(f,&disk)==1,
        "bundle update: final commit refusal preserves authoritative old manifest and removes only uncommitted staged files");
    native_plugin_bundle_install_fixture_failures(0);
    check(start(f,f.owner,1)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete),
        "bundle update: host extended-rename operation reaches bounded completion");
    const auto update=native_plugin_bundle_install_status(&f.owner);
    measurement("plugin_install_update_error=",static_cast<u32>(update.error));
    measurement("plugin_install_update_filesystem_error=",static_cast<u32>(update.filesystem.error));
    measurement("plugin_install_update_native_error=",update.filesystem.win32_error);
    measurement("plugin_install_update_committed=",update.committed);
    measurement("plugin_install_update_cleanup_pending=",update.cleanup_pending);
    check(update.committed&&update.ok()&&release(f.owner)&&installed(f,1)&&managed_count(f,&disk)==1,
        "bundle update: supported host replacement retires only authenticated prior managed DLL after publication");
    if(!update.committed){release(f.owner);return;}
    native_plugin_bundle_install_fixture_failures(2);
    check(start(f,f.owner,2)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).committed&&native_plugin_bundle_install_status(&f.owner).cleanup_pending&&!release(f.owner)&&managed_count(f,&disk)==2,
        "bundle update: failed cleanup retains two bounded versions and stable worker/file owner for explicit retry");
    native_plugin_bundle_install_acknowledge(&f.owner);
    check(WaitForSingleObject(f.owner.notification,0)==WAIT_TIMEOUT,"bundle update: consumed cleanup notification is not an idle signaled wait");
    native_plugin_bundle_install_fixture_failures(0);
    check(native_plugin_bundle_install_retry_cleanup(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&!native_plugin_bundle_install_status(&f.owner).cleanup_pending&&
        release(f.owner)&&installed(f,2)&&managed_count(f,&disk)==1&&disk==64,
        "bundle update: cleanup-only retry joins, closes retained pins and returns repeated-update storage to one version");
    for(u32 i=0;i<4;++i){check(start(f,f.owner,i%3),"bundle cancel: bounded early-cancel worker starts");native_plugin_bundle_install_cancel(&f.owner);
        check(await(f.owner,Phase::complete)&&!native_plugin_bundle_install_status(&f.owner).committed&&release(f.owner)&&installed(f,2),
            "bundle cancel: staging-publication race cannot admit commit or change the installed manifest");}
    wchar_t orphan_leaf[128]{},orphan[1024]{};managed_leaf(f.manifest[0],orphan_leaf);u8 data[64];for(u32 i=0;i<64;++i)data[i]=static_cast<u8>(i);
    const u8 wrong=0;
    check(join(orphan,f.destination,orphan_leaf)&&write(f,orphan,&wrong,1)&&start(f,f.owner,0)&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).error==Error::conflict&&release(f.owner)&&installed(f,2),
        "bundle conflict: occupied content-addressed destination with unrelated bytes is never overwritten or adopted");
    check(DeleteFileW(orphan)!=0&&write(f,orphan,data,64)&&start(f,f.owner,1)&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).error==Error::retained_limit&&release(f.owner)&&installed(f,2)&&managed_count(f,&disk)==2,
        "bundle retention: two historical managed files refuse a third before creation without namespace-guess cleanup");
    check(DeleteFileW(orphan)!=0,"bundle retention: fixture removes only its own explicitly created orphan");
    wchar_t canonical[128]{},canonical_path[1024]{},legacy_path[1024]{};
    constexpr wchar_t manifest_leaf[]=L"123456789abcdef0.maicord-plugin.json";
    for(u32 i=0;i<sizeof(manifest_leaf)/2;++i)canonical[i]=manifest_leaf[i];
    // A delete-sharing pin retains the old file, not ownership of its name.
    // Identical replacement bytes still have another identity and must fail
    // the last pathname check. These are only our exclusive fixture children.
    wchar_t displaced_path[1024]{};
    const bool staged=start(f,f.owner,0)&&await(f.owner,Phase::prepared);
    check(staged,"bundle conflict: replacement identity fixture reaches prepared update with retained old pin");
    if(!staged)return;
    const bool displaced=join(canonical_path,f.destination,canonical)&&join(displaced_path,f.destination,L"identity-fixture.old")&&
        MoveFileW(canonical_path,displaced_path)!=0;
    check(displaced,"bundle conflict: retained delete-sharing pin allows bounded fixture name change");
    if(!displaced)return;
    const bool substituted=CopyFileW(displaced_path,canonical_path,TRUE)!=0;
    check(substituted,"bundle conflict: exclusive fixture substitutes identical manifest bytes with a different file identity");
    if(!substituted)return;
    check(native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).error==Error::conflict&&!native_plugin_bundle_install_status(&f.owner).committed&&
        release(f.owner)&&installed(f,2)&&managed_count(f,&disk)==1,
        "bundle conflict: changed target identity refuses publication and preserves replacement even with identical contents");
    const bool restored=!f.owner.runtime&&DeleteFileW(canonical_path)!=0&&MoveFileW(displaced_path,canonical_path)!=0;
    check(restored&&installed(f,2),"bundle conflict: fixture restores only its own displaced original after retained pins release");
    if(!restored)return;
    const bool legacy_updated=join(canonical_path,f.destination,canonical)&&join(legacy_path,f.destination,L"legacy.maicord-plugin.json")&&
        MoveFileW(canonical_path,legacy_path)&&start(f,f.owner,0)&&await(f.owner,Phase::prepared)&&native_plugin_bundle_install_commit(&f.owner)==Error::none&&
        await(f.owner,Phase::complete)&&native_plugin_bundle_install_status(&f.owner).committed&&release(f.owner)&&installed(f,0)&&
        GetFileAttributesW(legacy_path)!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(canonical_path)==INVALID_FILE_ATTRIBUTES;
    check(legacy_updated,
        "bundle update: authenticated owner in a legacy manifest filename updates that exact filename without a duplicate manifest");
    if(!legacy_updated)return;
    // Selecting an installed legacy bundle must retain one compatible exact
    // manifest pin, not two handles whose conflicting share modes prevent
    // promotion. The unrelated legacy DLL itself remains caller-owned.
    wchar_t legacy_dll[1024]{},source_dll[1024]{};
    count=0;
    const bool promotion_fixture=join(legacy_dll,f.destination,L"input1.dll")&&join(source_dll,f.source,L"input1.dll")&&
        CopyFileW(source_dll,legacy_dll,TRUE)&&native_plugin_manifest_write(f.manifest[0],{record,4096},&count).ok()&&
        DeleteFileW(legacy_path)&&write(f,legacy_path,record,count);
    check(promotion_fixture,"bundle promotion: exclusive installed legacy manifest declares its original authenticated DLL");
    if(!promotion_fixture)return;
    check(native_plugin_bundle_install_start(&f.owner,{legacy_path,length(legacy_path)},{f.destination,length(f.destination)})==Error::none&&
        await(f.owner,Phase::prepared)&&native_plugin_bundle_install_status(&f.owner).update&&!native_plugin_bundle_install_status(&f.owner).idempotent&&
        native_plugin_bundle_install_commit(&f.owner)==Error::none&&await(f.owner,Phase::complete)&&
        native_plugin_bundle_install_status(&f.owner).ok()&&native_plugin_bundle_install_status(&f.owner).committed&&
        release(f.owner)&&installed(f,0)&&GetFileAttributesW(legacy_dll)!=INVALID_FILE_ATTRIBUTES&&
        GetFileAttributesW(legacy_path)!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(canonical_path)==INVALID_FILE_ATTRIBUTES,
        "bundle promotion: selected installed legacy source publishes managed leaf through retained exact pin and preserves legacy DLL");
}
} // namespace maicord

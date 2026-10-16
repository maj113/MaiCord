#include "test_output.hpp"
#include "maicord/platform/desktop_notifications.hpp"
#include "maicord/platform/notification_identity.hpp"
#include <NotificationActivationCallback.h>
#include <objbase.h>
namespace maicord {
namespace {
template<usize N> constexpr ConstSlice<wchar_t> wide(const wchar_t(&s)[N]) noexcept {return {s,static_cast<u32>(N-1)};}
bool contains(const DesktopNotificationXml& xml,Utf8View text) noexcept {
    for(u32 i=0;i+text.bytes<=xml.count;++i)if(bytes_equal(xml.bytes+i,text.data,text.bytes))return true;return false;
}
constexpr GUID kTestActivator{0x24a7b261,0xc3cf,0x42f8,{0x94,0x35,0x78,0x7a,0x3c,0xbc,0x9c,0x10}};
// Reads one REG_SZ below HKCU and compares it with the expected text.
bool registry_text(const wchar_t* key,const wchar_t* name,const wchar_t* expected) noexcept {
    wchar_t value[1100]{};DWORD bytes=sizeof(value)-sizeof(wchar_t),type=0;
    if(RegGetValueW(HKEY_CURRENT_USER,key,name,RRF_RT_REG_SZ,&type,value,&bytes)!=ERROR_SUCCESS)return false;
    u32 n=0;while(expected[n])++n;return bytes==(n+1)*sizeof(wchar_t)&&bytes_equal(value,expected,bytes);
}
bool registry_missing(const wchar_t* key,const wchar_t* name) noexcept {
    return RegGetValueW(HKEY_CURRENT_USER,key,name,RRF_RT_ANY,nullptr,nullptr,nullptr)==ERROR_FILE_NOT_FOUND;
}
u32 append(wchar_t* out,u32 at,const wchar_t* text) noexcept {while(*text)out[at++]=*text++;out[at]=0;return at;}
u32 append_hex(wchar_t* out,u32 at,u64 value) noexcept {for(u32 i=16;i--;)out[at++]=L"0123456789abcdef"[(value>>(i*4))&15];out[at]=0;return at;}
// Full create/check/repair/foreign/remove lifecycle against a scratch HKCU key
// standing in for Software\Classes: nothing is registered with COM or Windows,
// and the scratch tree is deleted whatever the outcome.
void test_notification_identity_registry(void (*check)(bool,const char*) noexcept) noexcept {
    wchar_t root[96]{},classes[128]{},aumid[256]{},server[256]{},exe[1024]{},command[1100]{};
    u32 r=append(root,0,L"Software\\MaiCordTests\\identity-");r=append_hex(root,r,(u64{GetCurrentProcessId()}<<32)|GetTickCount());
    u32 c=append(classes,0,root);c=append(classes,c,L"\\Classes");
    const u32 e=GetModuleFileNameW(nullptr,exe,1024);
    if(!e||e>=1024){check(false,"toast identity registry test locates its executable");return;}
    append(aumid,append(aumid,append(aumid,0,classes),L"\\AppUserModelId\\"),L"MaiCord.Probe.Registry");
    append(server,append(server,append(server,0,classes),L"\\CLSID\\{24A7B261-C3CF-42F8-9435-787A3CBC9C10}"),L"\\LocalServer32");
    append(command,append(command,append(command,0,L"\""),exe),L"\" --toast-activated");
    NotificationIdentityConfig id{wide(L"MaiCord.Probe.Registry"),kTestActivator,{exe,e},wide(L"MaiCord test"),
        wide(L"C:\\Probe\\notification-icon.png"),wide(L"0123456789abcdef0123456789abcdef"),{classes,c}};
    using E=NotificationIdentityError;
    check(notification_identity_check(id).error==E::absent,"toast identity registry: fresh scratch root is absent");
    const auto created=notification_identity_create(id);
    check(created.ok()&&created.registry_created&&notification_identity_check(id).ok(),
        "toast identity registry: create enrolls and the read-only check accepts it");
    check(registry_text(aumid,L"DisplayName",L"MaiCord test")&&registry_text(aumid,L"IconUri",L"C:\\Probe\\notification-icon.png")&&
        registry_text(aumid,L"CustomActivator",L"{24A7B261-C3CF-42F8-9435-787A3CBC9C10}")&&
        registry_text(server,nullptr,command)&&registry_text(server,L"ServerExecutable",exe),
        "toast identity registry: AppUserModelId names the activator and LocalServer32 launches the quoted executable");
    id.display_name=wide(L"MaiCord renamed");
    check(notification_identity_check(id).error==E::stale,"toast identity registry: drifted owned values are stale");
    const auto repaired=notification_identity_create(id);
    check(repaired.ok()&&!repaired.registry_created&&notification_identity_check(id).ok()&&registry_text(aumid,L"DisplayName",L"MaiCord renamed"),
        "toast identity registry: create repairs an owned identity in place");
    id.icon_path={};
    check(notification_identity_check(id).error==E::stale&&notification_identity_create(id).ok()&&registry_missing(aumid,L"IconUri")&&
        notification_identity_check(id).ok(),"toast identity registry: dropping the icon removes IconUri");
    auto foreign=id;foreign.ownership_nonce=wide(L"fedcba9876543210fedcba9876543210");
    const auto refused=notification_identity_create(foreign);
    check(notification_identity_check(foreign).error==E::foreign_resource&&refused.error==E::foreign_resource&&!refused.registry_created&&
        notification_identity_remove_owned(foreign).error==E::not_owned&&notification_identity_check(id).ok(),
        "toast identity registry: another owner's keys are never claimed, rewritten or removed");
    check(notification_identity_remove_owned(id).ok()&&notification_identity_check(id).error==E::absent&&
        notification_identity_remove_owned(id).error==E::absent,"toast identity registry: remove deletes exactly the owned keys");
    HKEY partial=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,aumid,0,nullptr,0,KEY_SET_VALUE,nullptr,&partial,nullptr)==ERROR_SUCCESS)RegCloseKey(partial);
    check(notification_identity_check(id).error==E::foreign_resource&&notification_identity_create(id).error==E::foreign_resource&&
        notification_identity_check(id).error==E::foreign_resource&&registry_missing(server,nullptr),
        "toast identity registry: an unstamped key at the AUMID is foreign and rolls back the COM key");
    RegDeleteTreeW(HKEY_CURRENT_USER,root);
    HKEY parent=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\MaiCordTests",0,KEY_QUERY_VALUE,&parent)==ERROR_SUCCESS){
        DWORD subkeys=1,values=1;RegQueryInfoKeyW(parent,nullptr,nullptr,nullptr,&subkeys,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);RegCloseKey(parent);
        if(!subkeys&&!values)RegDeleteKeyW(HKEY_CURRENT_USER,L"Software\\MaiCordTests");
    }
    HKEY gone=nullptr;const bool removed=RegOpenKeyExW(HKEY_CURRENT_USER,root,0,KEY_QUERY_VALUE,&gone)==ERROR_FILE_NOT_FOUND;if(gone)RegCloseKey(gone);
    check(removed,"toast identity registry: scratch root is deleted");
}
}
void test_native_notifications(void (*check)(bool,const char*) noexcept) noexcept {
    using ShowState=DesktopNotificationShowState;
    check(desktop_notification_show_state({})==ShowState::submitted,
        "toast pure Show classifier reports broker success as submitted rather than visible banner");
    const HRESULT recoverable[]={RPC_E_DISCONNECTED,CO_E_OBJNOTCONNECTED,
        HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE),static_cast<HRESULT>(RPC_S_SERVER_UNAVAILABLE)};
    bool retries=true;
    for(const auto h:recoverable)retries=retries && desktop_notification_show_state(
        {DesktopNotificationError::transport,static_cast<u32>(h)})==ShowState::retry_required;
    check(retries,"toast disconnect classifier requests a later turn even for a pre-Show broker failure");
    check(desktop_notification_show_state({DesktopNotificationError::transport,static_cast<u32>(E_ACCESSDENIED)})==ShowState::failed &&
        desktop_notification_show_state({DesktopNotificationError::invalid_input,static_cast<u32>(RPC_E_DISCONNECTED)})==ShowState::failed &&
        desktop_notification_show_state({DesktopNotificationError::closed,static_cast<u32>(CO_E_NOTINITIALIZED)})==ShowState::failed,
        "toast classifier never retries invalid payload, closed apartment, or denied identity");
    const auto closed_show=desktop_notifications_show_once(nullptr,{});
    check(closed_show.state==ShowState::failed && !closed_show.result.ok() && !closed_show.native_show_attempted,
        "toast closed owner records zero actual Show calls without native execution");
    DesktopNotificationActivation target{0x123456789abcdef0,0xfedcba9876543210,~u64{0}},parsed{};
    wchar_t args[61]{};u32 units{};
    check(desktop_notification_activation_format(target,{args,61},&units)&&units==60&&
        desktop_notification_activation_parse({args,units},&parsed)&&parsed.profile_id==target.profile_id&&
        parsed.channel_id==target.channel_id&&parsed.message_id==target.message_id,
        "toast canonical copied 16hex profile/channel/message activation roundtrip");
    wchar_t original[61];copy_bytes(original,args,sizeof(args));args[6]=L'A';
    check(!desktop_notification_activation_parse({args,60},&parsed)&&!parsed.profile_id,
        "toast activation rejects noncanonical uppercase and erases output");
    copy_bytes(args,original,sizeof(args));args[23]=L'm';
    check(!desktop_notification_activation_parse({args,60},&parsed)&&
        !desktop_notification_activation_parse(wide(L"https://discord.com/channels/1/2/3"),&parsed)&&
        !desktop_notification_activation_parse({original,59},&parsed)&&
        !desktop_notification_activation_format({0,2,3},{args,61},&units),
        "toast activation rejects wrong field order, URLs, truncation and zero identities");
    wchar_t hex[17]{};check(desktop_notification_hex_id(~u64{0},{hex,17})&&bytes_equal(hex,L"ffffffffffffffff",16*sizeof(wchar_t))&&
        !desktop_notification_hex_id(1,{hex,16}),"toast tags and groups exactly16hex even maximum snowflake");
    DesktopNotificationPayload p{target,utf8_literal("A<&\"' "),utf8_literal("body \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF"),{},true,false};
    DesktopNotificationXml xml{};
    check(desktop_notification_xml(p,&xml).ok()&&xml.count<4096&&xml.bytes[xml.count]==0&&
        contains(xml,utf8_literal("A&lt;&amp;&quot;&apos; "))&&contains(xml,p.body)&&
        contains(xml,utf8_literal("<audio silent=\"true\"/>")),"toast bounded XML escapes native text and preserves exact Unicode");
    p.avatar_file_uri=utf8_literal("file:///D:/MaiCord/avatar%20one.png");
    check(desktop_notification_xml(p,&xml).ok()&&contains(xml,utf8_literal("appLogoOverride")),"toast accepts caller-owned local avatar bridge without download");
    p.avatar_file_uri=utf8_literal("https://cdn.example/avatar.png");
    check(!desktop_notification_xml(p,&xml).ok()&&!xml.count,"toast transport rejects remote avatar URI");p.avatar_file_uri={};
    constexpr char nul[]={'a',0,'b'},bad[]={static_cast<char>(0xc0),static_cast<char>(0xaf)},control[]={1};
    p.title={nul,3};check(!desktop_notification_xml(p,&xml).ok(),"toast XML rejects embedded NUL before OS projection");
    p.title={bad,2};check(!desktop_notification_xml(p,&xml).ok(),"toast XML rejects overlong UTF8");
    p.title={control,1};check(!desktop_notification_xml(p,&xml).ok(),"toast XML rejects disallowed XML control");
    char large[1000];for(u32 i=0;i<1000;++i)large[i]='&';p.title={large,1000};
    check(desktop_notification_xml(p,&xml).error==DesktopNotificationError::capacity&&!xml.count,"toast escaped expansion fails atomically below4KiB");
    p.title=utf8_literal("probe");
    NotificationIdentityConfig identity{wide(L"MaiCord.Probe.Unit"),kTestActivator,wide(L"D:\\MaiCord\\MaiCord.exe"),
        wide(L"MaiCord test"),wide(L"C:\\Users\\Probe\\AppData\\Local\\MaiCord\\notification-icon.png"),wide(L"0123456789abcdef0123456789abcdef")};
    check(notification_identity_valid(identity),"toast identity validates concrete paths and nonce without registry mutation");
    identity.executable_path=wide(L"D:\\MaiCord\\..\\foreign.exe");check(!notification_identity_valid(identity),"toast identity refuses traversal");
    identity.executable_path=wide(L"D:\\MaiCord\\MaiCord.exe");identity.ownership_nonce=wide(L"short");check(!notification_identity_valid(identity),"toast identity refuses unowned short nonce");
    identity.ownership_nonce=wide(L"0123456789abcdef0123456789abcdef");identity.icon_path=wide(L"C:\\icon.exe");
    check(!notification_identity_valid(identity),"toast identity icon must be a .png or .ico image");
    identity.icon_path={};identity.classes_key=wide(L"Software\\\\Classes");
    check(!notification_identity_valid(identity)&&(identity.classes_key=wide(L"\\Software"),!notification_identity_valid(identity)),
        "toast identity refuses malformed registry roots");
    identity.classes_key={};
    // Read-only against the real Software\Classes: an unregistered test AUMID is
    // absent, never invalid input (the original Start Menu folder mismatch).
    check(notification_identity_check(identity).error==NotificationIdentityError::absent,
        "toast identity check of an unregistered valid identity reports absent, not invalid input");
    test_notification_identity_registry(check);
    DesktopNotifications owner{};DesktopNotificationConfig config{wide(L"MaiCord.Probe.Unit"),kTestActivator,nullptr,0,false};
    const bool initialized=desktop_notifications_initialize(&owner,config).ok();check(initialized,"toast nonemitting raw COM owner initializes");if(!initialized)return;
    check(desktop_notifications_owned_bytes(&owner)<=2048,"toast early factory and activation owner bounded independently of OS transport");
    auto rejected=p;rejected.body={control,1};
    const auto invalid_show=desktop_notifications_show_once(&owner,rejected);
    check(invalid_show.state==ShowState::failed && invalid_show.result.error==DesktopNotificationError::invalid_input &&
        !invalid_show.native_show_attempted,"toast invalid XML returns before any transport or actual Show attempt");
    const auto denied_show=desktop_notifications_show_once(&owner,p);
    check(denied_show.state==ShowState::failed && denied_show.result.error==DesktopNotificationError::transport &&
        denied_show.result.native_error==static_cast<u32>(E_ACCESSDENIED) && !denied_show.native_show_attempted,
        "toast nonenrolled transport failure records zero actual Show attempts without emitting");
    bool history=true;
    check(desktop_notifications_history_step(&owner,&history).ok()&&!history&&
        !desktop_notifications_history_pending(&owner),"toast empty history continuation performs no transport work");
    bool admitted=true;
    for(u32 i=0;i<8;++i)admitted=admitted&&desktop_notifications_remove_channel(&owner,1,100+i).ok();
    check(admitted&&desktop_notifications_history_pending(&owner)&&desktop_notifications_remove_channel(&owner,1,100).ok()&&
        desktop_notifications_remove_channel(&owner,1,200).error==DesktopNotificationError::capacity,
        "toast cleanup copies8 requests, coalesces repeated target and refuses capacity before broker work");
    const auto step=desktop_notifications_history_step(&owner,&history);
    check(!step.ok()&&history&&desktop_notifications_remove_channel(&owner,2,200).ok(),
        "toast nonenrolled history failure retires one request and releases its capacity");
    desktop_notifications_remove_profile(&owner,1);
    check(desktop_notifications_history_pending(&owner),"toast profile cancellation preserves other profile cleanup requests");
    desktop_notifications_remove_profile(&owner,2);
    check(!desktop_notifications_history_pending(&owner),"toast profile removal cancels its queued history before OS group call");
    IClassFactory* factory=nullptr;INotificationActivationCallback* callback=nullptr;
    const bool constructed=desktop_notifications_factory(&owner,reinterpret_cast<void**>(&factory)).ok()&&
        SUCCEEDED(factory->CreateInstance(nullptr,__uuidof(INotificationActivationCallback),reinterpret_cast<void**>(&callback)));
    check(constructed,"toast raw class factory exposes actual notification activation callback");
    if(constructed){
        copy_bytes(args,original,sizeof(args));
        check(callback->Activate(L"Foreign.App",args,nullptr,0)==E_INVALIDARG&&
            callback->Activate(L"MaiCord.Probe.Unit",L"mc1:p=broken",nullptr,0)==E_INVALIDARG,
            "toast callback validates AppUserModelID and canonical arguments");
        bool queued=true;for(u32 i=0;i<8;++i)queued=queued&&SUCCEEDED(callback->Activate(L"MaiCord.Probe.Unit",args,nullptr,0));
        check(queued&&FAILED(callback->Activate(L"MaiCord.Probe.Unit",args,nullptr,0)),"toast callback accepts8 and explicitly refuses overflow");
        args[6]=L'0';bool copied=true;for(u32 i=0;i<8;++i){DesktopNotificationActivation a{};copied=copied&&desktop_notifications_take_activation(&owner,&a)&&a.profile_id==target.profile_id&&a.channel_id==target.channel_id&&a.message_id==target.message_id;}
        check(copied&&!desktop_notifications_take_activation(&owner,&parsed),"toast queue owns copied records and drains exactly8");
        check(!desktop_notifications_show(&owner,p).ok(),"toast nonenrolled probe cannot emit an OS notification");
        copy_bytes(args,original,sizeof(args));callback->Activate(L"MaiCord.Probe.Unit",args,nullptr,0);desktop_notifications_discard_activations(&owner);
        check(!desktop_notifications_take_activation(&owner,&parsed),"toast profile switch discards pending copied activation");
    }
    desktop_notifications_remove_channel(&owner,9,99);
    desktop_notifications_release(&owner);
    check(!owner.runtime&&!desktop_notifications_owned_bytes(&owner),"toast teardown clears live owner while external COM references retain inert storage");
    if(callback){check(callback->Activate(L"MaiCord.Probe.Unit",original,nullptr,0)==CO_E_SERVER_STOPPING,
        "toast retained callback safely becomes inert after owner teardown");callback->Release();}
    if(factory){void* absent=nullptr;check(factory->CreateInstance(nullptr,__uuidof(INotificationActivationCallback),&absent)==CO_E_SERVER_STOPPING&&!absent,
        "toast retained factory refuses construction after teardown");check(factory->Release()==0,"toast final external COM reference releases bounded callback allocation");}
}
}

namespace maicord {
void test_toast_abi(void (*)(bool, const char*) noexcept) noexcept;
bool probe_desktop_notifications() noexcept;
bool probe_desktop_notifications_live(const wchar_t* report_path, bool toast) noexcept;
namespace native_notifications_entry {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_native_notifications_tests_entry() noexcept {
    maicord::test_native_notifications(maicord::native_notifications_entry::check);
    maicord::test_toast_abi(maicord::native_notifications_entry::check);
    maicord::native_notifications_entry::check(maicord::probe_desktop_notifications(), "native notification XML, activation queue and lifetime probe");
    // Manual only: registers a test identity with Windows (and, with the toast
    // flag, shows one real toast). The automated suite never sets these.
    wchar_t report[1024]{}, toast[4]{};
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_LIVE_REPORT", report, 1024);
    if (n && n < 1024) {
        const DWORD t = GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_LIVE_TOAST", toast, 4);
        maicord::native_notifications_entry::check(maicord::probe_desktop_notifications_live(report, t == 1 && toast[0] == L'1'),
            "live notification identity enrollment, activator registration and removal");
    }
    ExitProcess(maicord::native_notifications_entry::failures ? 1 : 0);
}

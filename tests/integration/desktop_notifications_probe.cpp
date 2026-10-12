#include "maicord/platform/notification_identity.hpp"
#include <NotificationActivationCallback.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <winstring.h>

namespace maicord {
namespace {
constexpr GUID kProbeActivator{0x8fbd2e90,0xe59d,0x4cab,{0xb1,0xdf,0x4b,0x98,0x11,0x77,0x84,0x32}};
template<usize N> constexpr ConstSlice<wchar_t> wide(const wchar_t(&v)[N]) noexcept {return {v,static_cast<u32>(N-1)};}
}
// Default dispatch is intentionally nonemitting and never enrolls an identity.
// It validates the OS XML parser, real COM callback factory, capacity and inert
// retained callback teardown. The caller owns all execution and receipts.
bool probe_desktop_notifications() noexcept {
    DesktopNotificationPayload payload{{1,2,3},utf8_literal("MaiCord native notification probe"),utf8_literal("Unicode \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF & <safe>"),{},true,false};
    DesktopNotificationXml text{};if(!desktop_notification_xml(payload,&text).ok())return false;
    const HRESULT apartment=RoInitialize(RO_INIT_SINGLETHREADED);if(FAILED(apartment))return false;
    HSTRING type=nullptr,content=nullptr;IInspectable* instance=nullptr;
    ABI::Windows::Data::Xml::Dom::IXmlDocumentIO* io=nullptr;
    wchar_t xml[4096];const int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.bytes,static_cast<int>(text.count),xml,4096);
    constexpr wchar_t name[]=L"Windows.Data.Xml.Dom.XmlDocument";
    HRESULT h=units?WindowsCreateString(name,sizeof(name)/sizeof(wchar_t)-1,&type):E_INVALIDARG;
    if(SUCCEEDED(h))h=WindowsCreateString(xml,units,&content);if(SUCCEEDED(h))h=RoActivateInstance(type,&instance);
    if(SUCCEEDED(h))h=instance->QueryInterface(__uuidof(ABI::Windows::Data::Xml::Dom::IXmlDocumentIO),reinterpret_cast<void**>(&io));
    if(SUCCEEDED(h))h=io->LoadXml(content);com_release(io);com_release(instance);if(content)WindowsDeleteString(content);if(type)WindowsDeleteString(type);RoUninitialize();if(FAILED(h))return false;
    DesktopNotifications owner{};DesktopNotificationConfig config{wide(L"MaiCord.Native.NonemittingProbe"),kProbeActivator,nullptr,0,false};
    if(!desktop_notifications_initialize(&owner,config).ok())return false;
    bool cleanup=true;
    bool cleanup_ok=desktop_notifications_history_step(&owner,&cleanup).ok()&&!cleanup;
    for(u32 i=0;i<8;++i)cleanup_ok=cleanup_ok&&desktop_notifications_remove_channel(&owner,1,10+i).ok();
    cleanup_ok=cleanup_ok&&desktop_notifications_remove_channel(&owner,1,10).ok()&&
        desktop_notifications_remove_channel(&owner,1,99).error==DesktopNotificationError::capacity;
    desktop_notifications_remove_profile(&owner,1);cleanup_ok=cleanup_ok&&!desktop_notifications_history_pending(&owner);
    IClassFactory* factory=nullptr;INotificationActivationCallback* callback=nullptr;
    bool ok=cleanup_ok&&desktop_notifications_factory(&owner,reinterpret_cast<void**>(&factory)).ok();
    if(ok)ok=SUCCEEDED(factory->CreateInstance(nullptr,__uuidof(INotificationActivationCallback),reinterpret_cast<void**>(&callback)));
    wchar_t args[61];u32 count{};ok=ok&&desktop_notification_activation_format(payload.target,{args,61},&count);
    if(ok){for(u32 i=0;i<8;++i)ok=ok&&SUCCEEDED(callback->Activate(L"MaiCord.Native.NonemittingProbe",args,nullptr,0));
        ok=ok&&FAILED(callback->Activate(L"MaiCord.Native.NonemittingProbe",args,nullptr,0));
        for(u32 i=0;i<8;++i){DesktopNotificationActivation a{};ok=ok&&desktop_notifications_take_activation(&owner,&a)&&a.profile_id==1&&a.channel_id==2&&a.message_id==3;}
    }
    desktop_notifications_release(&owner);if(callback){ok=ok&&callback->Activate(L"MaiCord.Native.NonemittingProbe",args,nullptr,0)==CO_E_SERVER_STOPPING;callback->Release();}
    if(factory)ok=(factory->Release()==0)&&ok;return ok&&!owner.runtime;
}
namespace {
constexpr GUID kLiveActivator{0x5d0c6f3e,0x8a41,0x4b8e,{0x9e,0x27,0x31,0xc4,0x6a,0x0b,0x52,0xd9}};
constexpr wchar_t kLiveSettings[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\MaiCord.SelfTest.Live";
struct Report {
    char text[1024]; u32 count;
    void add(const char* s) noexcept {while(*s&&count+1<sizeof(text))text[count++]=*s++;}
    void hex(const char* name,u32 v) noexcept {add(name);add("=0x");for(u32 i=8;i--;){const char d[2]={"0123456789abcdef"[(v>>(i*4))&15],0};add(d);}add("\r\n");}
};
bool key_exists(const wchar_t* key) noexcept {HKEY k=nullptr;const bool found=RegOpenKeyExW(HKEY_CURRENT_USER,key,0,KEY_QUERY_VALUE,&k)==ERROR_SUCCESS;if(k)RegCloseKey(k);return found;}
}
// Manual live check; the suite never runs it. With MAICORD_NOTIFICATION_LIVE_REPORT
// naming a report file, it enrolls a test AUMID/CLSID in the real
// HKCU\Software\Classes, registers the activator, reads Windows'
// NotificationSetting for it and removes everything again. A real toast is
// shown only when MAICORD_NOTIFICATION_LIVE_TOAST=1 is also set.
bool probe_desktop_notifications_live(const wchar_t* report_path,bool toast) noexcept {
    wchar_t exe[1024]{};const u32 e=GetModuleFileNameW(nullptr,exe,1024);if(!e||e>=1024)return false;
    const NotificationIdentityConfig id{wide(L"MaiCord.SelfTest.Live"),kLiveActivator,{exe,e},wide(L"MaiCord self-test"),{},
        wide(L"MaiCordSelfTestLiveNonce0001"),{}};
    Report report{};const bool settings_before=key_exists(kLiveSettings);
    const auto before=notification_identity_check(id);report.hex("check_before",static_cast<u32>(before.error));
    const auto created=notification_identity_create(id);report.hex("create_error",static_cast<u32>(created.error));report.hex("create_native",created.native_error);
    const auto checked=notification_identity_check(id);report.hex("check_after_create",static_cast<u32>(checked.error));
    DesktopNotifications owner{};u32 setting=~0u;
    auto init=checked.ok()?desktop_notifications_initialize(&owner,{id.app_id,id.activator_id,nullptr,0,true}):DesktopNotificationResult{DesktopNotificationError::identity,0};
    report.hex("activator_register",init.native_error);
    if(init.ok()){
        const auto queried=desktop_notifications_setting(&owner,&setting);report.hex("setting_hr",queried.native_error);report.hex("setting",setting);
        if(toast){
            const DesktopNotificationPayload payload{{1,2,3},utf8_literal("MaiCord notification self-test"),utf8_literal("Desktop notifications are working."),{},false,false};
            report.hex("show_hr",desktop_notifications_show(&owner,payload).native_error);Sleep(8000);
        }
        desktop_notifications_release(&owner);
    }
    const auto removed=notification_identity_remove_owned(id);report.hex("remove_error",static_cast<u32>(removed.error));
    const auto after=notification_identity_check(id);report.hex("check_after_remove",static_cast<u32>(after.error));
    // Windows may record per-app settings for the test AUMID; drop what this run added.
    const bool settings_added=!settings_before&&key_exists(kLiveSettings);report.hex("windows_settings_key_added",settings_added);
    if(settings_added)RegDeleteTreeW(HKEY_CURRENT_USER,kLiveSettings);
    const HANDLE file=CreateFileW(report_path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written{};WriteFile(file,report.text,report.count,&written,nullptr);CloseHandle(file);}
    return created.ok()&&checked.ok()&&init.ok()&&removed.ok()&&after.error==NotificationIdentityError::absent;
}
}

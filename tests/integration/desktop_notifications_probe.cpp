#include "maicord/platform/notification_identity.hpp"
#include "maicord/sys/registry.hpp"
#include "maicord/sys/winrt.hpp"
#include "../../src/platform/toast_abi.hpp"

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
    // The OS parser accepts the XML through the client's string references and activation calls.
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);if(FAILED(apartment))return false;
    namespace toast=toast_abi;toast::Object* factory_object=nullptr;toast::Object* instance=nullptr;toast::Object* io=nullptr;
    wchar_t xml[4096];const int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.bytes,static_cast<int>(text.count),xml,4095);
    if(units)xml[units]=0;toast::StringReference name(toast::kXmlDocumentClass),content(xml,static_cast<u32>(units));
    HRESULT h=units?RoGetActivationFactory(name.get(),toast::kIidActivationFactory,reinterpret_cast<void**>(&factory_object)):E_INVALIDARG;
    if(SUCCEEDED(h))h=toast::call(factory_object,toast::kActivateInstance,&instance);
    if(SUCCEEDED(h))h=toast::query(instance,toast::kIidXmlDocumentIO,&io);
    if(SUCCEEDED(h))h=toast::call(io,toast::kXmlLoad,content.get());
    toast::release(io);toast::release(instance);toast::release(factory_object);CoUninitialize();if(FAILED(h))return false;
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
}

#include "maicord/platform/desktop/notification_identity.hpp"
#include "maicord/sys/registry.hpp"
#include "maicord/sys/winrt.hpp"
#include "../../src/platform/desktop/toast_abi.hpp"

namespace maicord {
// The XML the client builds for a toast is accepted by the OS parser. The owner, callback and queue behaviour is
// tests/platform/desktop/native_notifications.cpp's.
bool probe_desktop_notifications() noexcept {
    DesktopNotificationPayload payload{{1,2,3},utf8_literal("MaiCord native notification probe"),utf8_literal("Unicode \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF & <safe>"),{},true};
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
    toast::release(io);toast::release(instance);toast::release(factory_object);CoUninitialize();
    return SUCCEEDED(h);
}
}

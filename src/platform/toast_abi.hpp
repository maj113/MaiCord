#pragma once

// The Windows.UI.Notifications and Windows.Data.Xml.Dom ABI interfaces that
// desktop_notifications.cpp calls, declared here because their SDK headers
// cost more than a second of front-end time. IIDs and vtable slots follow the
// SDK (tests/toast_abi.cpp checks both against it); methods MaiCord never
// calls only hold their slot and take untyped arguments.
#include <inspectable.h>

namespace maicord::toast_abi {

inline constexpr wchar_t kToastNotificationManagerClass[] =
    L"Windows.UI.Notifications.ToastNotificationManager";
inline constexpr wchar_t kToastNotificationClass[] = L"Windows.UI.Notifications.ToastNotification";
inline constexpr wchar_t kXmlDocumentClass[] = L"Windows.Data.Xml.Dom.XmlDocument";

// Windows.UI.Notifications.NotificationSetting.
enum NotificationSetting : int {
    NotificationSetting_Enabled = 0
};

struct IXmlElement : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_TagName(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAttribute(HSTRING name, HSTRING* value) = 0;
};

struct __declspec(uuid("f7f3a506-1e87-42d6-bcfb-b8c809fa5494")) IXmlDocument : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Doctype(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Implementation(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DocumentElement(IXmlElement** value) = 0;
};

struct __declspec(uuid("6cd0e74e-ee65-4489-9ebf-ca43e87ba637")) IXmlDocumentIO : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTRING xml) = 0;
};

struct IToastNotification : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Content(IXmlDocument** value) = 0;
};

struct __declspec(uuid("9dfb9fd1-143a-490e-90bf-b9fba7132de7")) IToastNotification2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Tag(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Group(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Group(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SuppressPopup(boolean value) = 0;
};

// IVectorView<ToastNotification*>.
struct IToastNotificationVectorView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, IToastNotification** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned* size) = 0;
};

struct IToastNotifier : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Show(IToastNotification* notification) = 0;
    virtual HRESULT STDMETHODCALLTYPE Hide(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Setting(NotificationSetting* value) = 0;
};

struct IToastNotificationHistory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RemoveGroup(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupWithId(HSTRING group, HSTRING application_id) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTagWithId(HSTRING tag, HSTRING group,
                                                             HSTRING application_id) = 0;
};

struct __declspec(uuid("3bc3d253-2f31-4092-9129-8ad5abf067da")) IToastNotificationHistory2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetHistory(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHistoryWithId(HSTRING application_id,
                                                       IToastNotificationVectorView** result) = 0;
};

struct __declspec(uuid("04124b20-82c6-4229-b109-fd9ed4662b53")) IToastNotificationFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(IXmlDocument* content,
                                                              IToastNotification** value) = 0;
};

struct __declspec(uuid("50ac103f-d235-4598-bbef-98fe4d1a3ad4")) IToastNotificationManagerStatics
    : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTRING application_id,
                                                                IToastNotifier** result) = 0;
};

struct __declspec(uuid("7ab93c52-0e48-4750-ba9d-1a4113981847")) IToastNotificationManagerStatics2
    : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_History(IToastNotificationHistory** value) = 0;
};

} // namespace maicord::toast_abi

// Checks src/platform/toast_abi.hpp against the SDK's WinRT headers: the
// interface IDs desktop_notifications.cpp queries, the vtable slot of every
// method it calls, the runtime class names and the enabled setting value.
#include "../src/platform/toast_abi.hpp"
#include "maicord/core/memory.hpp"
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>

namespace maicord {
namespace {
namespace sdk_toast = ABI::Windows::UI::Notifications;
namespace sdk_xml = ABI::Windows::Data::Xml::Dom;
namespace local = toast_abi;

// A stand-in object whose vtable slot N returns N, so a call through either
// declaration reports the slot that declaration reaches. The x64 convention
// lets each slot ignore the arguments it is passed.
using SlotFunction = HRESULT(STDMETHODCALLTYPE*)(void*);
template <int Slot> HRESULT STDMETHODCALLTYPE slot_index(void*) noexcept {
    return Slot;
}
template <int Count> struct SlotTable {
    static void fill(SlotFunction* table) noexcept {
        table[Count - 1] = &slot_index<Count - 1>;
        SlotTable<Count - 1>::fill(table);
    }
};
template <> struct SlotTable<0> {
    static void fill(SlotFunction*) noexcept {
    }
};
struct SlotObject {
    SlotFunction* vtable;
};
template <class T> T* as(SlotObject& object) noexcept {
    return reinterpret_cast<T*>(&object);
}
template <class Local, class Sdk> bool same_iid() noexcept {
    return bytes_equal(&__uuidof(Local), &__uuidof(Sdk), sizeof(GUID));
}
template <usize N, usize M>
bool same_name(const wchar_t (&local_name)[N], const wchar_t (&sdk_name)[M]) noexcept {
    return N == M && bytes_equal(local_name, sdk_name, N * sizeof(wchar_t));
}
static_assert(static_cast<int>(local::NotificationSetting_Enabled) ==
              static_cast<int>(sdk_toast::NotificationSetting_Enabled));
} // namespace

void test_toast_abi(void (*check)(bool, const char*) noexcept) noexcept {
    check(same_iid<local::IXmlDocument, sdk_xml::IXmlDocument>() &&
              same_iid<local::IXmlDocumentIO, sdk_xml::IXmlDocumentIO>() &&
              same_iid<local::IToastNotification2, sdk_toast::IToastNotification2>() &&
              same_iid<local::IToastNotificationHistory2, sdk_toast::IToastNotificationHistory2>() &&
              same_iid<local::IToastNotificationFactory, sdk_toast::IToastNotificationFactory>() &&
              same_iid<local::IToastNotificationManagerStatics,
                       sdk_toast::IToastNotificationManagerStatics>() &&
              same_iid<local::IToastNotificationManagerStatics2,
                       sdk_toast::IToastNotificationManagerStatics2>(),
          "toast ABI: interface IDs match the SDK");
    check(same_name(local::kToastNotificationManagerClass,
                    RuntimeClass_Windows_UI_Notifications_ToastNotificationManager) &&
              same_name(local::kToastNotificationClass,
                        RuntimeClass_Windows_UI_Notifications_ToastNotification) &&
              same_name(local::kXmlDocumentClass, RuntimeClass_Windows_Data_Xml_Dom_XmlDocument),
          "toast ABI: runtime class names match the SDK");

    SlotFunction table[24]{};
    SlotTable<24>::fill(table);
    SlotObject object{table};
    check(as<sdk_xml::IXmlElement>(object)->GetAttribute(nullptr, nullptr) ==
              as<local::IXmlElement>(object)->GetAttribute(nullptr, nullptr),
          "toast ABI: IXmlElement slots");
    check(as<sdk_xml::IXmlDocument>(object)->get_DocumentElement(nullptr) ==
              as<local::IXmlDocument>(object)->get_DocumentElement(nullptr),
          "toast ABI: IXmlDocument slots");
    check(as<sdk_xml::IXmlDocumentIO>(object)->LoadXml(nullptr) ==
              as<local::IXmlDocumentIO>(object)->LoadXml(nullptr),
          "toast ABI: IXmlDocumentIO slots");
    check(as<sdk_toast::IToastNotification>(object)->get_Content(nullptr) ==
              as<local::IToastNotification>(object)->get_Content(nullptr),
          "toast ABI: IToastNotification slots");
    auto* sdk_identity = as<sdk_toast::IToastNotification2>(object);
    auto* identity = as<local::IToastNotification2>(object);
    check(sdk_identity->put_Tag(nullptr) == identity->put_Tag(nullptr) &&
              sdk_identity->put_Group(nullptr) == identity->put_Group(nullptr) &&
              sdk_identity->put_SuppressPopup(0) == identity->put_SuppressPopup(0),
          "toast ABI: IToastNotification2 slots");
    auto* sdk_view = as<__FIVectorView_1_Windows__CUI__CNotifications__CToastNotification>(object);
    auto* view = as<local::IToastNotificationVectorView>(object);
    check(sdk_view->GetAt(0, nullptr) == view->GetAt(0, nullptr) &&
              sdk_view->get_Size(nullptr) == view->get_Size(nullptr),
          "toast ABI: IVectorView<ToastNotification> slots");
    auto* sdk_notifier = as<sdk_toast::IToastNotifier>(object);
    auto* notifier = as<local::IToastNotifier>(object);
    check(sdk_notifier->Show(nullptr) == notifier->Show(nullptr) &&
              sdk_notifier->get_Setting(nullptr) == notifier->get_Setting(nullptr),
          "toast ABI: IToastNotifier slots");
    auto* sdk_history = as<sdk_toast::IToastNotificationHistory>(object);
    auto* history = as<local::IToastNotificationHistory>(object);
    check(sdk_history->RemoveGroupWithId(nullptr, nullptr) ==
                  history->RemoveGroupWithId(nullptr, nullptr) &&
              sdk_history->RemoveGroupedTagWithId(nullptr, nullptr, nullptr) ==
                  history->RemoveGroupedTagWithId(nullptr, nullptr, nullptr),
          "toast ABI: IToastNotificationHistory slots");
    check(as<sdk_toast::IToastNotificationHistory2>(object)->GetHistoryWithId(nullptr, nullptr) ==
              as<local::IToastNotificationHistory2>(object)->GetHistoryWithId(nullptr, nullptr),
          "toast ABI: IToastNotificationHistory2 slots");
    check(as<sdk_toast::IToastNotificationFactory>(object)->CreateToastNotification(nullptr, nullptr) ==
              as<local::IToastNotificationFactory>(object)->CreateToastNotification(nullptr, nullptr),
          "toast ABI: IToastNotificationFactory slots");
    check(as<sdk_toast::IToastNotificationManagerStatics>(object)->CreateToastNotifierWithId(nullptr,
                                                                                             nullptr) ==
              as<local::IToastNotificationManagerStatics>(object)->CreateToastNotifierWithId(nullptr,
                                                                                             nullptr),
          "toast ABI: IToastNotificationManagerStatics slots");
    check(as<sdk_toast::IToastNotificationManagerStatics2>(object)->get_History(nullptr) ==
              as<local::IToastNotificationManagerStatics2>(object)->get_History(nullptr),
          "toast ABI: IToastNotificationManagerStatics2 slots");
}

} // namespace maicord

// Checks src/platform/toast_abi.hpp against the SDK's WinRT headers and
// combase: the interface IDs desktop_notifications.cpp queries, the vtable
// slot of every method it calls, the runtime class names, the enabled setting
// value and the HSTRING reference layout.
#include "../src/platform/toast_abi.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <activation.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>

namespace maicord {
namespace {
namespace sdk_toast = ABI::Windows::UI::Notifications;
namespace sdk_xml = ABI::Windows::Data::Xml::Dom;
namespace local = toast_abi;

// A stand-in object whose vtable slot N returns N, so a call through an SDK
// declaration reports the slot it reaches. The x64 convention lets each slot
// ignore the arguments it is passed.
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
bool at(HRESULT reached, u32 slot) noexcept {
    return reached == static_cast<HRESULT>(slot);
}
template <class Sdk> bool same_iid(const GUID& local_iid) noexcept {
    return bytes_equal(&local_iid, &__uuidof(Sdk), sizeof(GUID));
}
template <usize N, usize M>
bool same_name(const wchar_t (&local_name)[N], const wchar_t (&sdk_name)[M]) noexcept {
    return N == M && bytes_equal(local_name, sdk_name, N * sizeof(wchar_t));
}
static_assert(static_cast<int>(local::NotificationSetting_Enabled) ==
              static_cast<int>(sdk_toast::NotificationSetting_Enabled));

void test_string_reference(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr wchar_t text[] = L"Windows.UI.Notifications.ToastNotification";
    constexpr u32 length = sizeof(text) / sizeof(wchar_t) - 1;
    HSTRING_HEADER sdk_header{};
    HSTRING sdk_reference = nullptr;
    local::StringReference reference(text);
    check(SUCCEEDED(WindowsCreateStringReference(text, length, &sdk_header, &sdk_reference)) &&
              sdk_reference == reinterpret_cast<HSTRING>(&sdk_header) &&
              bytes_equal(&sdk_header, &reference.header, sizeof(sdk_header)),
          "toast ABI: an in-place string reference equals WindowsCreateStringReference's header");
    UINT32 read_length = 0;
    const wchar_t* read = WindowsGetStringRawBuffer(reference.get(), &read_length);
    HSTRING copy = nullptr;
    INT32 order = 1;
    check(read == text && read_length == length && WindowsGetStringLen(reference.get()) == length &&
              SUCCEEDED(WindowsDuplicateString(reference.get(), &copy)) && copy != reference.get() &&
              SUCCEEDED(WindowsCompareStringOrdinal(reference.get(), copy, &order)) && order == 0,
          "toast ABI: combase reads and copies an in-place string reference");
    const ConstSlice<wchar_t> owned = local::string_text(copy);
    read = WindowsGetStringRawBuffer(copy, &read_length);
    check(owned.data == read && owned.count == length && read_length == length &&
              bytes_equal(owned.data, text, length * sizeof(wchar_t)),
          "toast ABI: the text of an owned HSTRING reads from its header");
    check(SUCCEEDED(WindowsDeleteString(copy)) && !local::string_text(nullptr).data &&
              !local::string_text(nullptr).count,
          "toast ABI: a null HSTRING is the empty string");
}
} // namespace

void test_toast_abi(void (*check)(bool, const char*) noexcept) noexcept {
    check(same_iid<IActivationFactory>(local::kIidActivationFactory) &&
              same_iid<sdk_xml::IXmlDocument>(local::kIidXmlDocument) &&
              same_iid<sdk_xml::IXmlDocumentIO>(local::kIidXmlDocumentIO) &&
              same_iid<sdk_toast::IToastNotification2>(local::kIidToastNotification2) &&
              same_iid<sdk_toast::IToastNotificationHistory2>(local::kIidToastNotificationHistory2) &&
              same_iid<sdk_toast::IToastNotificationFactory>(local::kIidToastNotificationFactory) &&
              same_iid<sdk_toast::IToastNotificationManagerStatics>(local::kIidToastNotificationManagerStatics) &&
              same_iid<sdk_toast::IToastNotificationManagerStatics2>(local::kIidToastNotificationManagerStatics2),
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
    check(at(as<IActivationFactory>(object)->ActivateInstance(nullptr), local::kActivateInstance),
          "toast ABI: IActivationFactory slots");
    check(at(as<sdk_xml::IXmlElement>(object)->GetAttribute(nullptr, nullptr), local::kXmlGetAttribute),
          "toast ABI: IXmlElement slots");
    check(at(as<sdk_xml::IXmlDocument>(object)->get_DocumentElement(nullptr), local::kXmlDocumentElement),
          "toast ABI: IXmlDocument slots");
    check(at(as<sdk_xml::IXmlDocumentIO>(object)->LoadXml(nullptr), local::kXmlLoad),
          "toast ABI: IXmlDocumentIO slots");
    check(at(as<sdk_toast::IToastNotification>(object)->get_Content(nullptr), local::kToastContent),
          "toast ABI: IToastNotification slots");
    auto* identity = as<sdk_toast::IToastNotification2>(object);
    check(at(identity->put_Tag(nullptr), local::kToastPutTag) &&
              at(identity->put_Group(nullptr), local::kToastPutGroup) &&
              at(identity->put_SuppressPopup(0), local::kToastPutSuppressPopup),
          "toast ABI: IToastNotification2 slots");
    auto* view = as<__FIVectorView_1_Windows__CUI__CNotifications__CToastNotification>(object);
    check(at(view->GetAt(0, nullptr), local::kViewGetAt) && at(view->get_Size(nullptr), local::kViewSize),
          "toast ABI: IVectorView<ToastNotification> slots");
    auto* notifier = as<sdk_toast::IToastNotifier>(object);
    check(at(notifier->Show(nullptr), local::kNotifierShow) &&
              at(notifier->get_Setting(nullptr), local::kNotifierSetting),
          "toast ABI: IToastNotifier slots");
    check(at(local::call(reinterpret_cast<local::Object*>(&object), local::kNotifierSetting, nullptr),
             local::kNotifierSetting),
          "toast ABI: call() reaches the named slot");
    auto* history = as<sdk_toast::IToastNotificationHistory>(object);
    check(at(history->RemoveGroupWithId(nullptr, nullptr), local::kHistoryRemoveGroup) &&
              at(history->RemoveGroupedTagWithId(nullptr, nullptr, nullptr), local::kHistoryRemoveTag),
          "toast ABI: IToastNotificationHistory slots");
    check(at(as<sdk_toast::IToastNotificationHistory2>(object)->GetHistoryWithId(nullptr, nullptr),
             local::kHistoryGetWithId),
          "toast ABI: IToastNotificationHistory2 slots");
    check(at(as<sdk_toast::IToastNotificationFactory>(object)->CreateToastNotification(nullptr, nullptr),
             local::kCreateToast),
          "toast ABI: IToastNotificationFactory slots");
    check(at(as<sdk_toast::IToastNotificationManagerStatics>(object)->CreateToastNotifierWithId(nullptr, nullptr),
             local::kCreateNotifierWithId),
          "toast ABI: IToastNotificationManagerStatics slots");
    check(at(as<sdk_toast::IToastNotificationManagerStatics2>(object)->get_History(nullptr), local::kManagerHistory),
          "toast ABI: IToastNotificationManagerStatics2 slots");
    test_string_reference(check);
}

} // namespace maicord

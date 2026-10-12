#pragma once

// The Windows Runtime ABI that desktop_notifications.cpp calls, declared here
// instead of through the SDK projection headers (more than a second of
// front-end time each): objects are opaque and called by vtable slot, and
// strings go in as HSTRING references held in caller storage.
// tests/toast_abi.cpp checks every IID, slot and class name against the SDK
// and the reference layout against combase.
#include "maicord/core/slice.hpp"
#include <hstring.h>

// combase.dll exports, imported by name through the import library that
// cmake/libraries.cmake generates from src/platform/combase.def. The
// signatures match roapi.h and winstring.h.
extern "C" {
__declspec(dllimport) HRESULT __stdcall RoGetActivationFactory(HSTRING class_id, const GUID& iid,
                                                                void** factory);
HRESULT __stdcall WindowsDeleteString(HSTRING string);
}

namespace maicord::toast_abi {

// The HSTRING_HEADER that WindowsCreateStringReference fills. Owned HSTRINGs
// returned by Windows begin with the same length and text fields.
struct StringHeader {
    u32 flags; // 1: a reference; Windows never copies or frees the text
    u32 length;
    u32 reserved[2];
    const wchar_t* text;
};
static_assert(sizeof(StringHeader) == sizeof(HSTRING_HEADER));

// An HSTRING reference to caller text, which must stay valid for the duration
// of every call that receives it and be NUL-terminated at text[length].
struct StringReference {
    StringHeader header;
    StringReference(const wchar_t* text, u32 length) noexcept : header{1, length, {}, text} {
    }
    template <usize N>
    explicit StringReference(const wchar_t (&literal)[N]) noexcept : StringReference(literal, N - 1) {
    }
    HSTRING get() noexcept {
        return reinterpret_cast<HSTRING>(&header);
    }
};

// The text of an HSTRING Windows returned (null is the empty string), valid
// until the string is deleted.
inline ConstSlice<wchar_t> string_text(HSTRING string) noexcept {
    if (!string)
        return {};
    const auto* header = reinterpret_cast<const StringHeader*>(string);
    return {header->text, header->length};
}

// A COM or Windows Runtime interface pointer.
struct Object {
    void* const* vtable;
};

// Calls vtable slot `slot` of `object` with the arguments as given; each must
// already have the parameter's ABI type (interfaces and strings as pointers).
template <class... A> HRESULT call(Object* object, u32 slot, A... arguments) noexcept {
    static_assert((!__is_class(A) && ...), "COM arguments are scalars or pointers");
    using Method = HRESULT(__stdcall*)(Object*, A...);
    return reinterpret_cast<Method>(object->vtable[slot])(object, arguments...);
}
inline HRESULT query(Object* object, const GUID& iid, Object** out) noexcept {
    return call(object, 0, &iid, out);
}
inline void release(Object*& object) noexcept {
    if (object) {
        reinterpret_cast<unsigned long(__stdcall*)(Object*)>(object->vtable[2])(object);
        object = nullptr;
    }
}

inline constexpr wchar_t kToastNotificationManagerClass[] =
    L"Windows.UI.Notifications.ToastNotificationManager";
inline constexpr wchar_t kToastNotificationClass[] = L"Windows.UI.Notifications.ToastNotification";
inline constexpr wchar_t kXmlDocumentClass[] = L"Windows.Data.Xml.Dom.XmlDocument";

inline constexpr GUID kIidActivationFactory{
    0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr GUID kIidXmlDocument{
    0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};
inline constexpr GUID kIidXmlDocumentIO{
    0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
inline constexpr GUID kIidToastNotification2{
    0x9dfb9fd1, 0x143a, 0x490e, {0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7}};
inline constexpr GUID kIidToastNotificationHistory2{
    0x3bc3d253, 0x2f31, 0x4092, {0x91, 0x29, 0x8a, 0xd5, 0xab, 0xf0, 0x67, 0xda}};
inline constexpr GUID kIidToastNotificationFactory{
    0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
inline constexpr GUID kIidToastNotificationManagerStatics{
    0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
inline constexpr GUID kIidToastNotificationManagerStatics2{
    0x7ab93c52, 0x0e48, 0x4750, {0xba, 0x9d, 0x1a, 0x41, 0x13, 0x98, 0x18, 0x47}};

// Vtable slots with the ABI signature after `this`. IUnknown and IInspectable
// take slots 0-5, so each interface's own methods start at 6.
// IActivationFactory::ActivateInstance(IInspectable**)
inline constexpr u32 kActivateInstance = 6;
// IXmlDocumentIO::LoadXml(HSTRING)
inline constexpr u32 kXmlLoad = 6;
// IXmlDocument::get_DocumentElement(IXmlElement**)
inline constexpr u32 kXmlDocumentElement = 8;
// IXmlElement::GetAttribute(HSTRING name, HSTRING* value)
inline constexpr u32 kXmlGetAttribute = 7;
// IToastNotification::get_Content(IXmlDocument**)
inline constexpr u32 kToastContent = 6;
// IToastNotification2::put_Tag(HSTRING), put_Group(HSTRING), put_SuppressPopup(boolean)
inline constexpr u32 kToastPutTag = 6, kToastPutGroup = 8, kToastPutSuppressPopup = 10;
// IVectorView<ToastNotification*>::GetAt(unsigned, IToastNotification**), get_Size(unsigned*)
inline constexpr u32 kViewGetAt = 6, kViewSize = 7;
// IToastNotifier::Show(IToastNotification*), get_Setting(NotificationSetting*)
inline constexpr u32 kNotifierShow = 6, kNotifierSetting = 8;
// IToastNotificationHistory::RemoveGroupWithId(HSTRING group, HSTRING app_id),
// RemoveGroupedTagWithId(HSTRING tag, HSTRING group, HSTRING app_id)
inline constexpr u32 kHistoryRemoveGroup = 7, kHistoryRemoveTag = 8;
// IToastNotificationHistory2::GetHistoryWithId(HSTRING app_id, IVectorView<ToastNotification*>**)
inline constexpr u32 kHistoryGetWithId = 7;
// IToastNotificationFactory::CreateToastNotification(IXmlDocument*, IToastNotification**)
inline constexpr u32 kCreateToast = 6;
// IToastNotificationManagerStatics::CreateToastNotifierWithId(HSTRING app_id, IToastNotifier**)
inline constexpr u32 kCreateNotifierWithId = 7;
// IToastNotificationManagerStatics2::get_History(IToastNotificationHistory**)
inline constexpr u32 kManagerHistory = 6;

// Windows.UI.Notifications.NotificationSetting.
enum NotificationSetting : int {
    NotificationSetting_Enabled = 0
};

} // namespace maicord::toast_abi

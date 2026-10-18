#pragma once

#include "maicord/core/win32_records.hpp"

// Microsoft UI Automation server for one window, over UiAccessTree snapshots.
//
// Nothing is loaded until the first WM_GETOBJECT for UiaRootObjectId: until a
// UI Automation client asks for the window, UIAutomationCore.dll and
// OleAut32.dll stay unloaded and every query below is a field test. The
// provider has manual COM vtables. UI Automation calls server-side providers
// on its own threads, so every method that reads the snapshot is marshaled to
// the window thread with a bounded SendMessageTimeout (the message carries no
// pointer). It never runs actions inline; it forwards them to the host's act
// callback (which should defer them).

namespace maicord {
struct UiAccessPropertyChange;
struct UiAccessTree;
struct UiRect;
struct Utf8View;
enum class UiAccessAction : u8;

struct UiaHostCallbacks {
    void* context;
    // Current finished snapshot. The host rebuilds a stale snapshot here.
    const UiAccessTree* (*tree)(void* context) noexcept;
    // Requests an action for the node with this key. number carries a range
    // value or a UiAccessScroll; text a SetValue string. Must not block.
    bool (*act)(void* context, u64 key, UiAccessAction action, double number, Utf8View text) noexcept;
    // Physical pixels per DIP for the window.
    float (*scale)(void* context) noexcept;
    // Optional: client-DIP rectangles covering value bytes [start, end) of a
    // text node (one rectangle per line; a caret rectangle when start == end).
    // Returns the count written; zero leaves the range without geometry.
    u32 (*text_bounds)(void* context, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept;
};

enum class UiaNotificationKind : u8 {
    item_added,
    item_removed,
    action_completed,
    action_aborted,
    other,
};

enum class UiaNotificationProcessing : u8 {
    important_all,
    important_most_recent,
    all,
    most_recent,
    current_then_most_recent,
};

struct UiaRuntime;

struct UiaProviderHost {
    HWND window;
    UiaHostCallbacks callbacks;
    const UiaRuntime* runtime;
    void* root;
    // UI Automation calls providers from its own threads; every method that
    // reads the snapshot runs on this thread through call_message.
    DWORD thread;
    UINT call_message;
    void* volatile pending_call;
    SrwLock call_lock;
    volatile LONG elements; // live element objects (diagnostics/tests)
    volatile LONG ranges;   // live text range objects (diagnostics/tests)
    u32 requests;           // WM_GETOBJECT answers
    volatile LONG marshaled; // calls moved onto the window thread
    // Client pixels above the snapshot's DIP origin (a custom window caption).
    i32 content_offset_y;
    bool connected;
    bool load_failed;
};

bool uia_host_initialize(UiaProviderHost* host, HWND window, const UiaHostCallbacks& callbacks) noexcept;
// Answers WM_GETOBJECT(UiaRootObjectId), loading UI Automation on first use.
bool uia_host_window_message(UiaProviderHost* host, UINT message, WPARAM wparam, LPARAM lparam,
                             LRESULT* result) noexcept;
// Loads the system libraries without a window request (tests).
bool uia_host_load(UiaProviderHost* host) noexcept;
// False before the first request; afterwards UiaClientsAreListening.
bool uia_host_listening(const UiaProviderHost& host) noexcept;
bool uia_host_raise_focus(UiaProviderHost* host, u64 key) noexcept;
bool uia_host_raise_structure(UiaProviderHost* host, u64 key) noexcept;
// Raises the property event for a planned change; new values come from the
// current snapshot, old values from the change record.
bool uia_host_raise_property(UiaProviderHost* host, u64 key, const UiAccessPropertyChange& change) noexcept;
bool uia_host_raise_live_region(UiaProviderHost* host, u64 key) noexcept;
// Key 0 raises on the root. Returns false where notifications are unavailable.
bool uia_host_raise_notification(UiaProviderHost* host, u64 key, UiaNotificationKind kind,
                                 UiaNotificationProcessing processing, Utf8View text, Utf8View activity) noexcept;
// Disconnects every element; later calls on retained elements fail cleanly.
void uia_host_shutdown(UiaProviderHost* host) noexcept;
// New IRawElementProviderSimple reference for key (0 = root), as IUnknown*.
// The caller releases it. Used for events and tests.
void* uia_host_element(UiaProviderHost* host, u64 key) noexcept;

} // namespace maicord

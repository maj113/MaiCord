#pragma once

#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/text_composition.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {

// Semantic hints exposed through ITfInputScope. These guide Windows text
// services; validation remains owned by the bound model and its policy.
enum class SecureTextInputScope : u8 {
    text,
    login_name,
    digits,
};

struct SecureTextStoreGeometry {
    Win32Rect text_screen;
    Win32Rect window_screen;
};

// Geometry, range and point callbacks always receive non-null output pointers.
using SecureTextStoreGeometryFunction = bool (*)(void* context, SecureTextStoreGeometry* geometry) noexcept;
using SecureTextStoreChangedFunction = void (*)(void* context) noexcept;
using SecureTextStoreRangeGeometryFunction = bool (*)(void* context, u32 first, u32 last,
    RECT* rectangle, bool* clipped) noexcept;

// Screen point to model UTF-16. Outside text, return the nearest edge and inside=false.
using SecureTextStorePointFunction = bool (*)(void*, POINT, bool round_nearest,
    u32* position, bool* inside) noexcept;

struct SecureTextStoreConfig {
    HWND window;
    SecureTextStoreGeometryFunction geometry;
    SecureTextStoreChangedFunction changed;
    void* context;
    SecureTextStoreRangeGeometryFunction range_geometry;
    SecureTextStorePointFunction point;
};

// The COM implementation is hidden from the retained UI and the rest of the
// application. Only this pointer-sized handle crosses that boundary.
struct SecureTextStore {
    void* implementation;
};

// Native TSF composition state. ACP offsets are UTF-16 code-unit positions in
// the currently bound text target. Spans carry no TSF/COM types and may be
// projected into any widget or renderer presentation.
struct SecureTextComposition {
    TextCompositionSpan spans[kTextCompositionSpanCapacity];
    u32 start;
    u32 end;
    u32 span_count;
    u32 revision;
    bool active;
    bool complete;
};

bool secure_text_store_initialize(SecureTextStore* store, SecureTextStoreConfig config) noexcept;
void secure_text_store_release(SecureTextStore* store) noexcept;
// UI-thread boundary only, after external TSF calls and message dispatch return.
// Reentrant release/rebind clears borrowed targets immediately and keeps the native
// context alive until this drain. Failed cleanup is retried.
bool secure_text_store_process_deferred() noexcept;

// Attaches the store to the calling UI thread's TSF manager/document/context.
// The implementation loads system DLLs dynamically, so the login path's
// imports and startup are unaffected.
bool secure_text_store_attach(SecureTextStore* store) noexcept;
void secure_text_store_detach(SecureTextStore* store) noexcept;
bool secure_text_store_attached(const SecureTextStore& store) noexcept;
// Call before TranslateMessage/DispatchMessage. Consumed TSF key events must
// not also reach application editing or generate a second WM_CHAR.
bool secure_text_store_filter_message(SecureTextStore* store, const MSG& message) noexcept;

// Binding a different windowless field rebuilds the TSF context. This ends any
// prior composition and makes the new ITfInputScope value current.
bool secure_text_store_bind(SecureTextStore* store, SecureEdit* edit, SecureTextInputScope scope) noexcept;
bool secure_text_store_bind_target(SecureTextStore* store, TextInputTarget target, SecureTextInputScope scope) noexcept;
void secure_text_store_unbind(SecureTextStore* store) noexcept;
void secure_text_store_set_window_focus(SecureTextStore* store, bool focused) noexcept;

// Compare the bound SecureEdit with the last state observed by TSF and emit
// external text/selection notifications. Mutations initiated by TSF update the
// observation internally and are never echoed back to its sink.
void secure_text_store_synchronize(SecureTextStore* store) noexcept;
void secure_text_store_notify_layout(SecureTextStore* store) noexcept;
bool secure_text_store_composition(const SecureTextStore& store,
                                   SecureTextComposition* composition) noexcept;
// Whether a composition is active; false without a store. Skips the span copy.
bool secure_text_store_composing(const SecureTextStore& store) noexcept;

// Test-only seam returned as void* so Windows COM/TSF headers remain confined
// to the adapter implementation and tests that exercise the ABI.
void* secure_text_store_test_interface(SecureTextStore* store) noexcept;

} // namespace maicord

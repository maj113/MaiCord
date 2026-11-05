#pragma once

#include "maicord/ui/text/secure_edit.hpp"
#include "maicord/ui/text/composition.hpp"

#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord {

// Semantic hints exposed through ITfInputScope. These guide Windows text
// services; validation remains owned by the bound model and its policy.
enum class SecureTextInputScope : u8 {
    text,
    login_name,
    digits,
};

// Client-space pixels of the window the store is attached to (origin: top-left of the client area, caption rows
// included). The platform's text service turns them into what it needs (the Windows store makes screen rectangles,
// a Wayland input method wants surface-local ones).
struct SecureTextRect {
    i32 left, top, right, bottom;
};
struct SecureTextPoint {
    i32 x, y;
};
struct SecureTextStoreGeometry {
    SecureTextRect text;    // the text area
    SecureTextRect window;  // the area the text may appear in
};

// Geometry, range and point callbacks always receive non-null output pointers.
using SecureTextStoreGeometryFunction = bool (*)(void* context, SecureTextStoreGeometry* geometry) noexcept;
using SecureTextStoreChangedFunction = void (*)(void* context) noexcept;
using SecureTextStoreRangeGeometryFunction = bool (*)(void* context, u32 first, u32 last,
    SecureTextRect* rectangle, bool* clipped) noexcept;

// Client point to model UTF-16. Outside text, return the nearest edge and inside=false.
using SecureTextStorePointFunction = bool (*)(void*, SecureTextPoint, bool round_nearest,
    u32* position, bool* inside) noexcept;

struct SecureTextStoreConfig {
    NativeWindow window;  // the platform's window handle (window_surface_native)
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
// Call with a key_down or key_up event before the application handles it (WindowPumpHooks::key_prefilter).
// Consumed key events must not also reach application editing or generate a second character event. Only a
// text service that sees keys first (WindowCaps::key_prefilter: TSF) consumes any; the Windows store reads the
// system message in event.native, the others return false.
bool secure_text_store_filter_key(SecureTextStore* store, const WindowEvent& event) noexcept;
// An input method's transaction (ime_preedit, ime_commit, ime_delete_surrounding): applied to the store that holds the
// window's text input (the bound, attached, focused one) and shown through secure_text_store_composition like a TSF
// composition; that store's `changed` callback runs. True when a store took the event. Only platforms whose input
// method talks through events have one; the Windows store never takes any.
bool secure_text_store_ime_event(const WindowEvent& event) noexcept;

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

} // namespace maicord

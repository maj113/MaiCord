// The text store where there is no TSF: every field edits through the window's key and character events, so the store
// only keeps what the callers ask of it (bound, attached, focused) and reports no composition. Input methods arrive
// with the Wayland text-input protocol; this is where they would plug in.
#include "maicord/ui/text/secure_text_store.hpp"
#include "maicord/core/heap.hpp"

namespace maicord {
namespace {

struct Implementation {
    SecureTextStoreConfig config;
    bool attached;
    bool bound;
    bool focused;
};

Implementation* implementation(SecureTextStore* store) noexcept {
    return store ? static_cast<Implementation*>(store->implementation) : nullptr;
}

const Implementation* implementation(const SecureTextStore& store) noexcept {
    return static_cast<const Implementation*>(store.implementation);
}

} // namespace

bool secure_text_store_initialize(SecureTextStore* store, SecureTextStoreConfig config) noexcept {
    if (!store || store->implementation || !config.window)
        return false;
    auto* value = static_cast<Implementation*>(heap_allocate(sizeof(Implementation), true));
    if (!value)
        return false;
    value->config = config;
    store->implementation = value;
    return true;
}

void secure_text_store_release(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store)) {
        store->implementation = nullptr;
        heap_free(value);
    }
}

bool secure_text_store_process_deferred() noexcept {
    return true;
}

bool secure_text_store_attach(SecureTextStore* store) noexcept {
    auto* value = implementation(store);
    if (!value || !value->bound)
        return false;
    value->attached = true;
    return true;
}

void secure_text_store_detach(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store))
        value->attached = false;
}

bool secure_text_store_attached(const SecureTextStore& store) noexcept {
    const auto* value = implementation(store);
    return value && value->attached;
}

bool secure_text_store_filter_key(SecureTextStore*, const WindowEvent&) noexcept {
    return false;
}

bool secure_text_store_bind(SecureTextStore* store, SecureEdit* edit, SecureTextInputScope) noexcept {
    auto* value = implementation(store);
    if (!value || !edit)
        return false;
    value->bound = true;
    return true;
}

bool secure_text_store_bind_target(SecureTextStore* store, TextInputTarget, SecureTextInputScope) noexcept {
    auto* value = implementation(store);
    if (!value)
        return false;
    value->bound = true;
    return true;
}

void secure_text_store_unbind(SecureTextStore* store) noexcept {
    if (auto* value = implementation(store))
        value->bound = false;
}

void secure_text_store_set_window_focus(SecureTextStore* store, bool focused) noexcept {
    if (auto* value = implementation(store))
        value->focused = focused;
}

void secure_text_store_synchronize(SecureTextStore*) noexcept {}

void secure_text_store_notify_layout(SecureTextStore*) noexcept {}

bool secure_text_store_composition(const SecureTextStore&, SecureTextComposition*) noexcept {
    return false;
}

bool secure_text_store_composing(const SecureTextStore&) noexcept {
    return false;
}

} // namespace maicord

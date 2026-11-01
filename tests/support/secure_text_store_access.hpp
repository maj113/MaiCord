#pragma once
// The TSF text-store interface of a secure text store, for the tests that drive the ITextStoreACP ABI.
// The store's implementation handle points at its COM object, whose first base is ITextStoreACP, so the
// handle is that interface's address. Returned as void* so these headers need no COM declarations.
#include "maicord/ui/text/secure_text_store.hpp"

namespace maicord {
inline void* secure_text_store_test_interface(SecureTextStore* store) noexcept {
    return store ? store->implementation : nullptr;
}
} // namespace maicord

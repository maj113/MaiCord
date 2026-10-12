#pragma once
#include "maicord/model/message_composer.hpp"

namespace maicord {
using BoundedEditReady = bool (*)(void* context, u64 identity) noexcept;
// Borrowed ordinary text document. Release the native text binding before releasing
// the wrapper/model. A host callback may invalidate the scope; it must keep this
// wrapper alive until the callback returns. No storage is kept here.
struct BoundedEdit {
    MessageComposer* composer{};
    u64 identity{}, document_identity{}, binding_identity{};
    u32 maximum_codepoints{};
    bool multiline{};
    BoundedEditReady ready{};
    void* context{};
};
// Bind again after a borrowed-buffer refresh. Empty drafts are valid; final
// field validation is up to the caller. Each successful bind advances a native
// binding counter even when host/document identity is unchanged. Keep this same
// wrapper across binds; zero limits/identities and counter overflow are rejected.
// Existing text needs valid UTF-8, non-NUL scalars and a limited scalar count;
// the stricter insertion policy never rewrites existing characters.
bool bounded_edit_bind(BoundedEdit*, MessageComposer*, u32 maximum_codepoints,
                       bool multiline, u64 identity, BoundedEditReady ready = nullptr,
                       void* context = nullptr) noexcept;
TextInputTarget bounded_edit_input_target(BoundedEdit*) noexcept;
// begin_group reserves max(2048, 4 * model.capacity) history payload bytes on first
// use, capped at 16 KiB. If that much history is unavailable, begin_group fails;
// the base composer's 64 KiB default is never used by this wrapper.
} // namespace maicord

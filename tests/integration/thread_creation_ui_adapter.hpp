#pragma once
#include "application_thread_creation.hpp"
#include "application_text_form_state.hpp"
#include "maicord/protocol/thread_creation.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "reply_draft_ui_adapter.hpp"
namespace maicord {
bool thread_creation_ui_adapter_begin(HWND, D3D11Renderer&, UiTextMeasureCache&, bool (*)(void*) noexcept, void*) noexcept;
void thread_creation_ui_adapter_end() noexcept;
bool thread_creation_ui_adapter_active() noexcept;
bool thread_creation_ui_adapter_refresh() noexcept;
D3D11Renderer& thread_creation_ui_adapter_renderer(D3D11Renderer&) noexcept;
void thread_creation_ui_adapter_viewport(D3D11Renderer&, const UiScene&) noexcept;
bool thread_creation_ui_adapter_open() noexcept;
bool thread_creation_ui_adapter_menu() noexcept;
bool thread_creation_ui_adapter_input(UiInputKind, u32, bool repeated = false, bool control = false, bool shift = false) noexcept;
bool thread_creation_ui_adapter_pointer(TextFormElement) noexcept;
bool thread_creation_ui_adapter_pointer_down(TextFormElement) noexcept;
bool thread_creation_ui_adapter_pointer_up() noexcept;
bool thread_creation_ui_adapter_synchronize() noexcept;
bool thread_creation_ui_adapter_submit(u64) noexcept;
bool thread_creation_ui_adapter_response(u16, Utf8View = {}, RestClientError = RestClientError::none,
    u64 exact_id = 0) noexcept;
bool thread_creation_ui_adapter_flight(ThreadCreationRequest*, u64*) noexcept;
u32 thread_creation_ui_adapter_submissions() noexcept;
u32 thread_creation_ui_adapter_cancellations() noexcept;
app_detail::ApplicationTextForm* thread_creation_ui_adapter_form() noexcept;
bool thread_creation_ui_adapter_retire(u8 reason) noexcept;
} // namespace maicord

#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
enum class GatewayClientEventKind : u8;
}

namespace maicord::app_detail {
// Composer text commands and slowmode, just before a send. *handled means the
// typed text must not be sent (it was consumed, or slowmode is cooling down).
bool application_composer_prepare_send(bool* handled) noexcept;
void application_composer_note_sent(u64 channel_id) noexcept;
// A predicted channel slowmode cooldown is running. Current bypass permission
// or disabled slowmode retires it; authoritative server retry deadlines are separate.
// A reduced duration caps the remaining prediction; increases never extend it.
bool application_composer_slowmode_active(u64 channel_id) noexcept;
// Connection strip above the composer, debounced; slowmode countdown otherwise.
void application_composer_gateway_event(GatewayClientEventKind kind) noexcept;
Utf8View application_composer_status_notice() noexcept;
bool application_composer_status_notice_is_error() noexcept;
// Tints a jumped-to message: 200 ms fade in, hold, 200 ms fade out.
void application_message_highlight_begin(u64 message_id) noexcept;
u64 application_message_interaction_next_wake(u64 now_ms) noexcept;
void application_message_interaction_tick(u64 now_ms) noexcept;

} // namespace maicord::app_detail

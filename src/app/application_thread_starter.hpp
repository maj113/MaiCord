#pragma once
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"

namespace maicord {
struct ChatStateStore;
} // namespace maicord

namespace maicord::app_detail {
// Fallback when a message has no decoded `thread`: a known thread channel
// with the message's id under the same parent supplies the starter card.
// Returns true when a rich record changed. Pure over its inputs.
bool thread_starter_project(const MessageStore&, const ChatStateStore&, MessageRichStore*) noexcept;
// Runs the projection when the message or channel stores changed.
bool application_thread_starters_sync() noexcept;
} // namespace maicord::app_detail

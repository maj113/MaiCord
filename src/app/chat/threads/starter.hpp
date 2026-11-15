#pragma once

namespace maicord::app_detail {
// A message with no decoded `thread` gets its starter card from a known thread channel that has the id of the
// message under the same parent. Runs when the message or channel stores changed. Returns true when a rich record
// changed.
bool application_thread_starters_sync() noexcept;
} // namespace maicord::app_detail

#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Frame-loop driver for composer misspelling marks (Settings > Chat >
// Spellcheck). COM runs only here and from the context menu, never from
// WM_CHAR. Release before COM teardown at shutdown.
void application_composer_spellcheck_tick(u64 now_ms) noexcept;
void application_composer_spellcheck_release() noexcept;
} // namespace maicord::app_detail

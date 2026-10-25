#pragma once

#include "maicord/app/account_paths.hpp"

namespace maicord::app_detail {

// The previous session's authenticated shell (startup_shell.hpp), drawn from
// the account's encrypted file before READY. While it is provisional
// (g_app.startup_shell_provisional) the shell is view-mostly: the composer
// keeps typed text but does not send, reads are not acknowledged and no
// member data is requested. READY replaces the stores wholesale.

// Worker start: reads, unwraps and decodes the account's shell on a pool
// thread, and owns its later writes.
void application_startup_shell_begin(const AccountPaths& paths, u64 profile_id) noexcept;
// Account UI attach: waits for that read, fills the stores and folders from it
// and places its selection. True when the shell is shown.
bool application_startup_shell_apply() noexcept;
// A fresh READY is about to be applied. True when the shell's account ID
// differed and was dropped so READY can establish its own.
bool application_startup_shell_gateway_ready(u64 self_user_id) noexcept;
// READY replaced the stores: the shell ends and its collapsed categories
// come back.
void application_startup_shell_ready() noexcept;
// True once after READY ended a shell that replaced the restoring view.
bool application_startup_shell_take_release() noexcept;
// Records selection changes and starts the due (debounced) write.
void application_startup_shell_pump(u64 now_ms) noexcept;
u64 application_startup_shell_next_wake() noexcept;
// Account stop: writes the current shell (not while logging out), waits for
// it and releases the key. A shell READY has not replaced stays provisional
// through a restart; releasing the account UI ends it.
void application_startup_shell_stop() noexcept;

} // namespace maicord::app_detail

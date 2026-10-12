#pragma once

#include "maicord/app/client_preferences.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

struct ClientPreferencesWriter;

// Create lazily on the first explicit preference edit. No disk IO occurs here
// or in submit/poll. A null path selects the current user's normal preference
// file; a non-null path is copied and is the only persistence destination.
// Failure leaves *writer unchanged and reports a Win32 last-error value.
bool client_preferences_writer_create(ClientPreferencesWriter** writer,
                                      const wchar_t* path = nullptr) noexcept;

// One in-progress write and one replaceable newest snapshot, with no growing
// queue. Accepted revisions start at 1 and increase monotonically. Invalid or
// stopping writers reject submissions without changing *revision.
bool client_preferences_writer_submit(ClientPreferencesWriter* writer,
                                      const ClientPreferences& preferences,
                                      u64* revision) noexcept;

// Borrowed manual-reset event for the main wait set; never close this handle.
HANDLE client_preferences_writer_completion_event(const ClientPreferencesWriter* writer) noexcept;

// Consume the newest completion and reset its event while holding the same
// lock used when a completion is committed. ready=false leaves revision/result untouched.
// Coalesced revisions need not complete individually. Failure reports Win32
// last-error and leaves outputs unchanged. All output pointers are required.
bool client_preferences_writer_poll(ClientPreferencesWriter* writer,
                                    u64* completed_revision,
                                    ClientPreferencesResult* result,
                                    bool* ready) noexcept;

// Reject further submissions and finish writing the newest accepted snapshot,
// then join. Success releases all handles/storage and nulls *writer. A timeout
// or wait failure keeps the pointer and every resource for a later join; never
// free it separately. A null *writer is already stopped. A successful join says
// nothing about the last save result: consume the completion before stopping if
// it is needed. The owning thread serializes create/poll/stop. Concurrent
// submissions are supported, but none may overlap stop or use the pointer
// after a successful join.
bool client_preferences_writer_stop(ClientPreferencesWriter** writer,
                                    u32 timeout_ms) noexcept;

} // namespace maicord

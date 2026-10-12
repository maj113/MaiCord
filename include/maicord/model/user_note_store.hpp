#pragma once
#include "maicord/protocol/user_social.hpp"

namespace maicord {
// Notes about other users, as the profile card shows them. Typing edits a
// local draft; the draft is saved once typing pauses (or the field loses
// focus), one request at a time per user. A save in flight never loses a
// newer draft, and Gateway updates never overwrite unsaved typing.
inline constexpr u32 kUserNoteCacheCapacity = 32;
inline constexpr u64 kUserNoteSaveDelayMs = 800;
inline constexpr u64 kUserNoteRetryDelayMs = 5'000;
inline constexpr u8 kUserNoteMaximumAttempts = 3;

enum class UserNoteState : u8 { empty, loading, loaded, failed };
enum class UserNoteWork : u8 { none, load, save };

struct UserNoteEntry {
    u64 user_id;
    u64 due_ms;    // next save of a dirty draft; zero when not scheduled
    u32 last_used;
    u16 bytes, draft_bytes, sent_bytes;
    UserNoteState state;
    u8 attempts;
    bool dirty, saving, load_pending, save_failed;
    char text[kUserNoteMaximumBytes];  // last known server value
    char draft[kUserNoteMaximumBytes]; // what the field shows while dirty
    char sent[kUserNoteMaximumBytes];  // the draft a save in flight carries
};
struct UserNoteStore {
    UserNoteEntry entries[kUserNoteCacheCapacity];
    u32 count, clock, revision;
};
struct UserNoteNext {
    u64 user_id;
    u64 due_ms; // zero: ready now
    UserNoteWork work;
};

const UserNoteEntry* user_note_find(const UserNoteStore&, u64 user_id) noexcept;
// The text the field shows: the draft while dirty, else the server value.
Utf8View user_note_text(const UserNoteStore&, u64 user_id) noexcept;
// Ensures an entry and schedules one GET unless the note is known or loading.
bool user_note_request(UserNoteStore*, u64 user_id) noexcept;
// Replaces the draft; the save is due kUserNoteSaveDelayMs after the last edit.
bool user_note_edit(UserNoteStore*, u64 user_id, Utf8View text, u64 now_ms) noexcept;
// Makes a pending draft due now (blur, card closed, Enter).
void user_note_flush(UserNoteStore*, u64 user_id, u64 now_ms) noexcept;
// The next request: due saves first, then loads. due_ms is the earliest future
// deadline when nothing is ready (work none).
UserNoteNext user_note_next(const UserNoteStore&, u64 now_ms) noexcept;
// Copies the draft into the in-flight slot; output borrows it until finished.
bool user_note_save_begin(UserNoteStore*, u64 user_id, Utf8View* output) noexcept;
void user_note_save_finished(UserNoteStore*, u64 user_id, bool ok, u64 now_ms) noexcept;
void user_note_load_begin(UserNoteStore*, u64 user_id) noexcept;
// ok=false marks the note failed (it can be requested again); a 404 is an
// empty, known note and arrives as ok with empty text.
void user_note_loaded(UserNoteStore*, u64 user_id, bool ok, Utf8View text) noexcept;
// USER_NOTE_UPDATE: authoritative unless a newer local draft is pending.
void user_note_gateway(UserNoteStore*, u64 user_id, Utf8View text) noexcept;
void user_note_reset(UserNoteStore*) noexcept;
} // namespace maicord

#include "maicord/model/user_note_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
UserNoteEntry* find(UserNoteStore* store, u64 user_id) noexcept {
    for (u32 i = 0; i < store->count; ++i)
        if (store->entries[i].user_id == user_id)
            return store->entries + i;
    return nullptr;
}
bool same(const char* a, u32 a_bytes, const char* b, u32 b_bytes) noexcept {
    if (a_bytes != b_bytes)
        return false;
    for (u32 i = 0; i < a_bytes; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
// Evicts the least recently used entry that carries no unsaved work.
UserNoteEntry* acquire(UserNoteStore* store, u64 user_id) noexcept {
    if (!user_id)
        return nullptr;
    if (auto* entry = find(store, user_id)) {
        entry->last_used = ++store->clock;
        return entry;
    }
    UserNoteEntry* slot = nullptr;
    if (store->count < kUserNoteCacheCapacity)
        slot = store->entries + store->count++;
    else
        for (u32 i = 0; i < store->count; ++i) {
            auto& candidate = store->entries[i];
            if (candidate.dirty || candidate.saving || candidate.state == UserNoteState::loading)
                continue;
            if (!slot || candidate.last_used < slot->last_used)
                slot = &candidate;
        }
    if (!slot)
        return nullptr;
    clear_bytes(slot, static_cast<u32>(sizeof(*slot)));
    slot->user_id = user_id;
    slot->last_used = ++store->clock;
    return slot;
}
void settle_draft(UserNoteEntry& e) noexcept {
    if (e.dirty && same(e.draft, e.draft_bytes, e.text, e.bytes)) {
        e.dirty = false;
        e.due_ms = 0;
        e.attempts = 0;
        e.save_failed = false;
    }
}
} // namespace

const UserNoteEntry* user_note_find(const UserNoteStore& store, u64 user_id) noexcept {
    return find(const_cast<UserNoteStore*>(&store), user_id);
}

Utf8View user_note_text(const UserNoteStore& store, u64 user_id) noexcept {
    const auto* e = user_note_find(store, user_id);
    if (!e)
        return {};
    return e->dirty ? Utf8View{e->draft, e->draft_bytes} : Utf8View{e->text, e->bytes};
}

bool user_note_request(UserNoteStore* store, u64 user_id) noexcept {
    if (!store)
        return false;
    auto* e = acquire(store, user_id);
    if (!e)
        return false;
    if (e->state == UserNoteState::empty || e->state == UserNoteState::failed)
        e->load_pending = true;
    return true;
}

bool user_note_edit(UserNoteStore* store, u64 user_id, Utf8View text, u64 now_ms) noexcept {
    if (!store || !user_note_valid(text))
        return false;
    auto* e = acquire(store, user_id);
    if (!e)
        return false;
    copy_bytes(e->draft, text.data, text.bytes);
    e->draft_bytes = static_cast<u16>(text.bytes);
    e->dirty = true;
    e->attempts = 0;
    e->save_failed = false;
    e->due_ms = now_ms + kUserNoteSaveDelayMs;
    // Typing back to the saved value cancels a pending save (but not one in flight).
    if (!e->saving && e->state == UserNoteState::loaded)
        settle_draft(*e);
    ++store->revision;
    return true;
}

void user_note_flush(UserNoteStore* store, u64 user_id, u64 now_ms) noexcept {
    auto* e = store ? find(store, user_id) : nullptr;
    if (e && e->dirty && !e->save_failed && (!e->due_ms || e->due_ms > now_ms))
        e->due_ms = now_ms;
}

UserNoteNext user_note_next(const UserNoteStore& store, u64 now_ms) noexcept {
    UserNoteNext next{};
    u64 earliest = 0;
    for (u32 i = 0; i < store.count; ++i) {
        const auto& e = store.entries[i];
        // Never save over a load in flight; its response settles the draft.
        if (!e.dirty || e.saving || !e.due_ms || e.state == UserNoteState::loading)
            continue;
        if (e.due_ms <= now_ms)
            return {e.user_id, 0, UserNoteWork::save};
        earliest = earliest ? minimum(earliest, e.due_ms) : e.due_ms;
    }
    for (u32 i = 0; i < store.count; ++i) {
        const auto& e = store.entries[i];
        if (e.load_pending && e.state != UserNoteState::loading && !e.saving)
            return {e.user_id, 0, UserNoteWork::load};
    }
    next.due_ms = earliest;
    return next;
}

bool user_note_save_begin(UserNoteStore* store, u64 user_id, Utf8View* output) noexcept {
    auto* e = store ? find(store, user_id) : nullptr;
    if (!e || !e->dirty || e->saving || !output)
        return false;
    copy_bytes(e->sent, e->draft, e->draft_bytes);
    e->sent_bytes = e->draft_bytes;
    e->saving = true;
    e->due_ms = 0;
    ++e->attempts;
    *output = {e->sent, e->sent_bytes};
    return true;
}

void user_note_save_finished(UserNoteStore* store, u64 user_id, bool ok, u64 now_ms) noexcept {
    auto* e = store ? find(store, user_id) : nullptr;
    if (!e || !e->saving)
        return;
    e->saving = false;
    ++store->revision;
    const bool newer_draft = !same(e->draft, e->draft_bytes, e->sent, e->sent_bytes);
    if (ok) {
        copy_bytes(e->text, e->sent, e->sent_bytes);
        e->bytes = e->sent_bytes;
        e->state = UserNoteState::loaded;
        e->load_pending = false;
        e->attempts = 0;
        if (newer_draft) {
            // Typed while saving: save the newer draft after the usual pause.
            if (!e->due_ms)
                e->due_ms = now_ms + kUserNoteSaveDelayMs;
        } else
            settle_draft(*e);
        return;
    }
    if (newer_draft) {
        e->attempts = 0;
        if (!e->due_ms)
            e->due_ms = now_ms + kUserNoteSaveDelayMs;
        return;
    }
    if (e->attempts >= kUserNoteMaximumAttempts) {
        e->save_failed = true;
        e->due_ms = 0;
        return;
    }
    e->due_ms = now_ms + kUserNoteRetryDelayMs;
}

void user_note_load_begin(UserNoteStore* store, u64 user_id) noexcept {
    auto* e = store ? find(store, user_id) : nullptr;
    if (!e)
        return;
    e->load_pending = false;
    e->state = UserNoteState::loading;
}

void user_note_loaded(UserNoteStore* store, u64 user_id, bool ok, Utf8View text) noexcept {
    auto* e = store ? find(store, user_id) : nullptr;
    if (!e || e->state != UserNoteState::loading)
        return;
    ++store->revision;
    if (!ok || !user_note_valid(text)) {
        e->state = UserNoteState::failed;
        return;
    }
    copy_bytes(e->text, text.data, text.bytes);
    e->bytes = static_cast<u16>(text.bytes);
    e->state = UserNoteState::loaded;
    settle_draft(*e);
}

void user_note_gateway(UserNoteStore* store, u64 user_id, Utf8View text) noexcept {
    if (!store || !user_note_valid(text))
        return;
    auto* e = find(store, user_id);
    if (!e)
        return; // Notes are cached on demand; unknown users load when shown.
    copy_bytes(e->text, text.data, text.bytes);
    e->bytes = static_cast<u16>(text.bytes);
    if (e->state != UserNoteState::loading)
        e->state = UserNoteState::loaded;
    e->load_pending = false;
    if (!e->saving)
        settle_draft(*e);
    ++store->revision;
}

void user_note_reset(UserNoteStore* store) noexcept {
    if (!store)
        return;
    const u32 revision = store->revision + 1;
    clear_bytes(store, static_cast<u32>(sizeof(*store)));
    store->revision = revision;
}
} // namespace maicord

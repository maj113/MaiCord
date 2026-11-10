#pragma once
#include "../shell/state.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord::app_detail {

// What the composer strip's and the forum form's attachment thumbnails keep in common. An account media lane decodes
// a local file into the preview atlas: request admits the decode and polls it every 100 ms, retire asks the lane to
// drop it. Each owner keeps a table of entries (a type derived from this, one per draft), and calls changed() when a
// helper below reports that what is shown changed.
struct LocalPreviewEntry {
    AccountMediaLocalPreviewRequest request;
    void* instance;
    u64 profile, poll_at;
    bool occupied, admitted, ready, failed, retired;
};

template <class Entry, u32 Count> __forceinline Entry* local_preview_find(Entry (&entries)[Count], MediaAssetKey key) noexcept {
    for (auto& entry : entries)
        if (entry.occupied && media_asset_key_equal(entry.request.key, key))
            return &entry;
    return nullptr;
}

// The earliest poll time of any entry, or 0 when none polls.
template <class Entry, u32 Count> __forceinline u64 local_preview_next_wake(const Entry (&entries)[Count]) noexcept {
    u64 wake = 0;
    for (const auto& entry : entries)
        if (entry.occupied && entry.poll_at && (!wake || entry.poll_at < wake))
            wake = entry.poll_at;
    return wake;
}

// Marks the entry retired and has the lane drop its decode; the slot is freed at once when the lane never admitted it
// or its account is gone, else once the lane has retired it (polled until then).
template <class Entry> __forceinline void local_preview_retire(Entry& entry, u64 now, bool actor_current) noexcept {
    entry.retired = true;
    entry.ready = false;
    if (!entry.admitted || !actor_current) {
        reset_value(&entry);
        return;
    }
    AccountMediaLocalPreviewRequest exact{};
    exact.key = entry.request.key;
    exact.owner_scope = entry.request.owner_scope;
    exact.owner_generation = entry.request.owner_generation;
    const auto result = account_module_media_local_preview(entry.instance, AccountLocalPreviewOperation::retire,
                                                              &exact, 0, 0);
    if (result.state == AccountMediaRequestState::pending)
        entry.poll_at = deadline_after(now, 100);
    else
        reset_value(&entry);
}

// The decoded thumbnail reached the atlas. True when the entry was not ready before.
inline bool local_preview_uploaded(LocalPreviewEntry& entry) noexcept {
    const bool changed = !entry.ready;
    entry.ready = true;
    entry.poll_at = 0;
    return changed;
}

// The lane refused the source: the file card stays. True when that changed what is shown.
inline bool local_preview_rejected(LocalPreviewEntry& entry) noexcept {
    const bool changed = !entry.failed || entry.ready;
    entry.ready = false;
    entry.failed = true;
    entry.poll_at = 0;
    return changed;
}

// A ready thumbnail the atlas evicted under pressure decodes again from the retained source. True when it did.
inline bool local_preview_evicted(LocalPreviewEntry& entry) noexcept {
    if (!entry.ready || !g_app.renderer.preview_media_atlas ||
        media_atlas_peek(g_app.renderer.preview_media_atlas, entry.request.key).ok())
        return false;
    entry.ready = false;
    entry.poll_at = 0;
    return true;
}

// Asks the lane to decode the entry and schedules the next poll. True when the lane refused the request outright (the
// entry failed).
inline bool local_preview_admit(LocalPreviewEntry& entry, u64 now, u64 unix_now) noexcept {
    const auto result = account_module_media_local_preview(entry.instance, AccountLocalPreviewOperation::request,
                                                              &entry.request, now, unix_now);
    if (result.state == AccountMediaRequestState::queued || result.state == AccountMediaRequestState::pending ||
        result.state == AccountMediaRequestState::ready) {
        entry.admitted = true;
    } else if (result.state != AccountMediaRequestState::busy) {
        entry.failed = true;
        entry.poll_at = 0;
        return true;
    }
    entry.poll_at = deadline_after(now, 100);
    return false;
}

} // namespace maicord::app_detail

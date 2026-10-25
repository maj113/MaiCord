#include "application_forum_preview.hpp"
#include "application_forum_attachments.hpp"
#include "application_forum_form.hpp"
#include "application_state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
inline constexpr u32 kPreviewSlots = 24; // Ten current rows plus eight draining lanes and spare slots.
struct PreviewEntry {
    AccountMediaLocalPreviewRequest request;
    ApplicationForumPostTicket ticket;
    void* instance;
    u64 account_scope, profile, self, poll_at;
    bool occupied, admitted, ready, failed, retired;
};
struct PreviewOwner {
    PreviewEntry entries[kPreviewSlots];
    u64 admissions;
};
PreviewOwner* owner;
u32 next_generation;
u32 publication;
void changed() noexcept {
    if (++publication == 0)
        ++publication;
    application_request_frame();
}
bool actor_current(const PreviewEntry& entry) noexcept {
    return g_app.account_ui && g_app.account.instance == entry.instance &&
           g_app.plugin_account_scope_serial == entry.account_scope &&
           g_app.account.status.profile_id == entry.profile &&
           g_app.account_ui->chat_state.self_user_id == entry.self;
}
PreviewEntry* find(MediaAssetKey key) noexcept {
    if (owner)
        for (auto& entry : owner->entries)
            if (entry.occupied && media_asset_key_equal(entry.request.key, key))
                return &entry;
    return nullptr;
}
bool visible(u64 id) noexcept {
    auto* scene = application_forum_form_scene();
    if (!application_forum_form_active() || !scene)
        return false;
    const auto& list = scene->attachment_list;
    for (u32 i = 0; i < list.rows.count; ++i)
        if (list.rows[i].id == id && !list.rows[i].spoiler) {
            UiRect clip{};
            const auto rect = ui_attachment_draft_list_preview(list, i, &clip);
            return maximum(rect.left, clip.left) < minimum(rect.right, clip.right) &&
                   maximum(rect.top, clip.top) < minimum(rect.bottom, clip.bottom);
        }
    return false;
}
bool source_current(const PreviewEntry& entry) noexcept {
    if (entry.retired || !actor_current(entry) ||
        !application_forum_attachments_current(entry.ticket, entry.request.key.id))
        return false;
    if (entry.failed && !entry.request.snapshot.bytes)
        return true;
    UploadSource content{};
    FileSnapshot snapshot{};
    ConstSlice<wchar_t> path{};
    return application_forum_attachments_source(entry.ticket, entry.request.key.id, &content, &snapshot,
                                                &path) &&
           local_draft_preview_snapshot_equal(snapshot, entry.request.snapshot) &&
           content.context == entry.request.content.context && content.ops == entry.request.content.ops &&
           content.bytes == entry.request.content.bytes;
}
MediaAttachmentFormat format(Utf8View filename) noexcept {
    if (filename_ends_with(filename, utf8_literal(".png")))
        return MediaAttachmentFormat::png;
    if (filename_ends_with(filename, utf8_literal(".webp")))
        return MediaAttachmentFormat::webp;
    if (filename_ends_with(filename, utf8_literal(".jpg")) || filename_ends_with(filename, utf8_literal(".jpeg")))
        return MediaAttachmentFormat::jpeg;
    if (filename_ends_with(filename, utf8_literal(".gif")))
        return MediaAttachmentFormat::gif;
    return MediaAttachmentFormat::unknown;
}
void retire(PreviewEntry& entry, u64 now) noexcept {
    if (!entry.retired)
        changed();
    entry.retired = true;
    entry.ready = false;
    if (!entry.admitted || !actor_current(entry) || !g_app.account.api.media_local_preview) {
        reset_value(&entry);
        return;
    }
    AccountMediaLocalPreviewRequest exact{};
    exact.key = entry.request.key;
    exact.owner_scope = entry.request.owner_scope;
    exact.owner_generation = entry.request.owner_generation;
    const auto result = g_app.account.api.media_local_preview(
        entry.instance, AccountLocalPreviewOperation::retire, &exact, 0, 0);
    if (result.state == AccountMediaRequestState::pending)
        entry.poll_at = deadline_after(now, 100);
    else
        reset_value(&entry);
}
} // namespace
ApplicationForumPreview application_forum_preview_view(const ApplicationForumPostTicket& ticket,
                                                       u64 id) noexcept {
    if (owner)
        for (const auto& entry : owner->entries)
            if (entry.occupied && entry.request.key.id == id && entry.ticket == ticket &&
                source_current(entry))
                return {media_asset_key_valid(entry.request.key) ? entry.request.key : MediaAssetKey{},
                        entry.ready, entry.failed};
    return {};
}
bool application_forum_preview_key_current(MediaAssetKey key) noexcept {
    const auto* entry = find(key);
    return entry && !entry->failed && source_current(*entry);
}
bool application_forum_preview_frame_current(const AccountMediaFrame& frame) noexcept {
    const auto* entry = find(frame.key);
    return entry && !entry->failed && source_current(*entry) &&
           local_draft_preview_frame_matches(frame, entry->request);
}
void application_forum_preview_frame_uploaded(const AccountMediaFrame& frame) noexcept {
    if (!application_forum_preview_frame_current(frame))
        return;
    auto* entry = find(frame.key);
    if (!entry->ready)
        changed();
    entry->ready = true;
    entry->poll_at = 0;
}
void application_forum_preview_rejected(MediaAssetKey key) noexcept {
    if (auto* entry = find(key)) {
        if (!entry->failed || entry->ready)
            changed();
        entry->ready = false;
        entry->failed = true;
        entry->poll_at = 0;
    }
}
bool application_forum_preview_request(u64 now, u64 unix_now) noexcept {
    if (!now || !unix_now)
        return true;
    if (owner)
        for (auto& entry : owner->entries) {
            if (!entry.occupied)
                continue;
            if (!actor_current(entry)) {
                changed();
                reset_value(&entry);
                continue;
            }
            if (!source_current(entry)) {
                if (!entry.retired || now >= entry.poll_at)
                    retire(entry, now);
            }
        }
    const auto post = application_forum_post_view();
    if (!post.available || !application_forum_post_current(post.ticket) || !g_app.account.instance ||
        !g_app.account.api.media_local_preview || !g_app.account_ui)
        return true;
    const auto view = application_forum_attachments_view(post.ticket);
    if (!view.rows.count)
        return true;
    if (!owner)
        owner = static_cast<PreviewOwner*>(
            VirtualAlloc(nullptr, sizeof(PreviewOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return true; // Optional thumbnail allocation never blocks posting.
    for (u32 row_index = 0; row_index < view.rows.count; ++row_index) {
        const auto& row = view.rows[row_index];
        const bool in_view = visible(row.id);
        PreviewEntry* entry = nullptr;
        for (auto& slot : owner->entries)
            if (slot.occupied && !slot.retired && slot.request.key.id == row.id &&
                slot.ticket == post.ticket) {
                entry = &slot;
                break;
            }
        if (!in_view && (!entry || !entry->admitted || entry->ready || entry->failed))
            continue;
        if (!entry) {
            for (auto& slot : owner->entries)
                if (!slot.occupied) {
                    entry = &slot;
                    break;
                }
            if (!entry || next_generation == 0xffffffffu)
                continue;
            const auto kind = format(row.filename);
            const auto key = media_asset_local_draft_preview(row.id, ++next_generation, kind);
            reset_value(entry);
            changed();
            entry->occupied = true;
            entry->ticket = post.ticket;
            entry->request.key = key;
            if (!key.id)
                entry->request.key = {row.id, next_generation, MediaAssetKind::local_draft_preview, 0};
            entry->instance = g_app.account.instance;
            entry->account_scope = g_app.plugin_account_scope_serial;
            entry->profile = g_app.account.status.profile_id;
            entry->self = g_app.account_ui->chat_state.self_user_id;
            entry->request.owner_scope = post.ticket.serial;
            entry->request.owner_generation = post.ticket.publication_revision;
            const bool source_found =
                application_forum_attachments_source(post.ticket, row.id, &entry->request.content,
                                                     &entry->request.snapshot, &entry->request.path);
            if (!media_asset_key_valid(key) || !source_found ||
                !local_draft_preview_request_valid(entry->request)) {
                // Retain the opaque draft id even for unsupported placeholders.
                entry->request.key.id = row.id;
                entry->failed = true;
                continue;
            }
        }
        if (in_view && entry->ready && g_app.renderer.preview_media_atlas &&
            !media_atlas_peek(g_app.renderer.preview_media_atlas, entry->request.key).ok()) {
            changed();
            entry->ready = false;
            entry->poll_at = 0;
        }
        if (entry->ready || entry->failed || now < entry->poll_at)
            continue;
        const auto result = g_app.account.api.media_local_preview(
            entry->instance, AccountLocalPreviewOperation::request, &entry->request, now, unix_now);
        if (result.state == AccountMediaRequestState::queued) {
            entry->admitted = true;
            ++owner->admissions;
        } else if (result.state == AccountMediaRequestState::pending ||
                   result.state == AccountMediaRequestState::ready)
            entry->admitted = true;
        else if (result.state != AccountMediaRequestState::busy) {
            changed();
            entry->failed = true;
            entry->poll_at = 0;
            continue;
        }
        entry->poll_at = deadline_after(now, 100);
    }
    return true;
}
void application_forum_preview_retire(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    if (owner)
        for (auto& entry : owner->entries)
            if (entry.occupied && entry.ticket == ticket && (!id || entry.request.key.id == id))
                retire(entry, clock_monotonic_milliseconds());
}
void application_forum_preview_release() noexcept {
    if (!owner)
        return;
    for (auto& entry : owner->entries)
        if (entry.occupied)
            retire(entry, clock_monotonic_milliseconds());
    // Lanes own retained sources and copied paths independently after admission.
    secure_clear_bytes(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
    changed();
}
u32 application_forum_preview_revision() noexcept {
    return publication;
}
u64 application_forum_preview_next_wake() noexcept {
    u64 wake = 0;
    if (owner)
        for (const auto& entry : owner->entries)
            if (entry.occupied && entry.poll_at && (!wake || entry.poll_at < wake))
                wake = entry.poll_at;
    return wake;
}
ApplicationForumPreviewMetrics application_forum_preview_metrics() noexcept {
    ApplicationForumPreviewMetrics value{};
    if (!owner)
        return value;
    value.owner_bytes = sizeof(*owner);
    value.committed_bytes = (sizeof(*owner) + 4095u) & ~4095u;
    value.admissions = owner->admissions;
    for (const auto& entry : owner->entries)
        if (entry.occupied) {
            ++value.active;
            value.pending += entry.admitted && !entry.ready && !entry.failed;
            value.ready += entry.ready;
            value.failed += entry.failed;
        }
    return value;
}
} // namespace maicord::app_detail

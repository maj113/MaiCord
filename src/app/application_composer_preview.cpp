#include "application_composer_preview.hpp"
#include "application_attachments_state.hpp"
#include "application_state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"

#include "maicord/core/memory.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
inline constexpr u32 kComposerPreviewSlots = 16; // Ten visible tiles plus draining retirements.
struct ComposerPreviewEntry {
    AccountMediaLocalPreviewRequest request;
    void* instance;
    u64 draft_id, profile, poll_at;
    bool occupied, admitted, ready, failed, retired;
};
struct ComposerPreviewOwner {
    ComposerPreviewEntry entries[kComposerPreviewSlots];
    u64 admissions, retirements;
    u32 next_generation;
};
ComposerPreviewOwner* owner;
u32 publication;

void changed() noexcept {
    if (++publication == 0)
        ++publication;
    application_request_frame();
}
bool actor_current(const ComposerPreviewEntry& entry) noexcept {
    return g_app.account_ui && g_app.account.instance == entry.instance &&
           g_app.account.status.profile_id == entry.profile;
}
ComposerPreviewEntry* find(MediaAssetKey key) noexcept {
    if (owner)
        for (auto& entry : owner->entries)
            if (entry.occupied && media_asset_key_equal(entry.request.key, key))
                return &entry;
    return nullptr;
}
// The draft still exists unsent in the active channel with the same source.
bool source_current(const ComposerPreviewEntry& entry) noexcept {
    const auto* state = g_app.attachments;
    if (entry.retired || !actor_current(entry) || !state || state->closing ||
        state->profile_id != entry.profile)
        return false;
    const auto* record = message_attachment_drafts_get(&state->drafts, entry.draft_id);
    if (!record || record->phase == MessageAttachmentDraftPhase::queued ||
        record->channel_id != g_app.account_ui->active_channel_id)
        return false;
    if (entry.failed && !entry.request.snapshot.bytes)
        return true;
    return local_draft_preview_snapshot_equal(record->source, entry.request.snapshot) &&
           record->content.context == entry.request.content.context &&
           record->content.ops == entry.request.content.ops;
}
void unpin(MediaAssetKey key) noexcept {
    if (g_app.renderer.preview_media_atlas && media_asset_key_valid(key))
        media_atlas_unpin(g_app.renderer.preview_media_atlas, key);
}
void retire(ComposerPreviewEntry& entry, u64 now) noexcept {
    if (!entry.retired) {
        changed();
        unpin(entry.request.key);
        ++owner->retirements;
    }
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
    const auto result = g_app.account.api.media_local_preview(entry.instance, AccountLocalPreviewOperation::retire,
                                                              &exact, 0, 0);
    if (result.state == AccountMediaRequestState::pending)
        entry.poll_at = deadline_after(now, 100);
    else
        reset_value(&entry);
}
} // namespace

MediaAttachmentFormat application_composer_preview_format(Utf8View filename) noexcept {
    char lowered[8];
    const Utf8View extension{lowered, ui_attachment_file_extension(filename, {lowered, 8})};
    if (utf8_equal(extension, utf8_literal("png")))
        return MediaAttachmentFormat::png;
    if (utf8_equal(extension, utf8_literal("jpg")) || utf8_equal(extension, utf8_literal("jpeg")) ||
        utf8_equal(extension, utf8_literal("jfif")))
        return MediaAttachmentFormat::jpeg;
    if (utf8_equal(extension, utf8_literal("webp")))
        return MediaAttachmentFormat::webp;
    if (utf8_equal(extension, utf8_literal("gif")))
        return MediaAttachmentFormat::gif;
    // H.264/HEVC movies get a GPU-decoded first-frame poster.
    if (utf8_equal(extension, utf8_literal("mp4")) || utf8_equal(extension, utf8_literal("m4v")) ||
        utf8_equal(extension, utf8_literal("mov")))
        return MediaAttachmentFormat::mp4;
    return MediaAttachmentFormat::unknown;
}

ApplicationComposerPreview application_composer_preview_view(u64 draft_id) noexcept {
    if (owner && draft_id)
        for (const auto& entry : owner->entries)
            if (entry.occupied && !entry.retired && entry.draft_id == draft_id && source_current(entry))
                return {media_asset_key_valid(entry.request.key) ? entry.request.key : MediaAssetKey{}, entry.ready,
                        entry.failed};
    return {};
}

bool application_composer_preview_key_current(MediaAssetKey key) noexcept {
    const auto* entry = find(key);
    return entry && !entry->failed && source_current(*entry);
}
bool application_composer_preview_frame_current(const AccountMediaFrame& frame) noexcept {
    const auto* entry = find(frame.key);
    return entry && !entry->failed && source_current(*entry) &&
           local_draft_preview_frame_matches(frame, entry->request);
}
void application_composer_preview_frame_uploaded(const AccountMediaFrame& frame) noexcept {
    if (!application_composer_preview_frame_current(frame))
        return;
    auto* entry = find(frame.key);
    if (!entry->ready)
        changed();
    entry->ready = true;
    entry->poll_at = 0;
}
void application_composer_preview_rejected(MediaAssetKey key) noexcept {
    if (auto* entry = find(key)) {
        if (!entry->failed || entry->ready)
            changed();
        entry->ready = false;
        entry->failed = true;
        entry->poll_at = 0;
    }
}

void application_composer_preview_deferred(MediaAssetKey key, u64 retry_at_ms) noexcept {
    if (auto* entry = find(key); entry && !entry->ready && !entry->failed && retry_at_ms > entry->poll_at)
        entry->poll_at = retry_at_ms;
}

bool application_composer_preview_request(u64 now, u64 unix_now) noexcept {
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
            if (!source_current(entry) && (!entry.retired || now >= entry.poll_at))
                retire(entry, now);
        }
    auto* state = g_app.attachments;
    if (!state || state->closing || !g_app.account.instance || !g_app.account.api.media_local_preview ||
        !g_app.account_ui || state->profile_id != g_app.account.status.profile_id)
        return true;
    u64 ids[kAttachmentUploadMaximumCount]{};
    const u32 count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id,
                                                        {ids, kAttachmentUploadMaximumCount});
    if (!count)
        return true;
    if (!owner) {
        owner = static_cast<ComposerPreviewOwner*>(
            VirtualAlloc(nullptr, sizeof(ComposerPreviewOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner)
            return true; // Thumbnails are optional; uploads never wait for them.
    }
    for (u32 index = 0; index < minimum(count, kAttachmentUploadMaximumCount); ++index) {
        const u64 id = ids[index];
        const auto* record = message_attachment_drafts_get(&state->drafts, id);
        if (!record)
            continue;
        ComposerPreviewEntry* entry = nullptr;
        for (auto& slot : owner->entries)
            if (slot.occupied && !slot.retired && slot.draft_id == id) {
                entry = &slot;
                break;
            }
        if (!entry) {
            for (auto& slot : owner->entries)
                if (!slot.occupied) {
                    entry = &slot;
                    break;
                }
            if (!entry || owner->next_generation >= 0x7fffffffu)
                continue;
            reset_value(entry);
            changed();
            entry->occupied = true;
            entry->draft_id = id;
            entry->instance = g_app.account.instance;
            entry->profile = g_app.account.status.profile_id;
            // The high variant bit separates composer keys from forum keys and
            // asks the codec lane for a bounded thumbnail.
            const u32 generation = ++owner->next_generation | kLocalDraftPreviewThumbnailVariant;
            const auto format =
                application_composer_preview_format(message_attachment_drafts_filename(&state->drafts, id));
            const auto key = media_asset_local_draft_preview(id, generation, format);
            entry->request.key = key.id ? key : MediaAssetKey{id, generation, MediaAssetKind::local_draft_preview, 0};
            entry->request.owner_scope = 0xC0A7000000000000ull | (state->profile_id & 0xffffffffffffull);
            entry->request.owner_generation = generation;
            entry->request.snapshot = record->source;
            entry->request.content = record->content;
            if (format == MediaAttachmentFormat::mp4)
                entry->request.video_device = g_app.renderer.graphics.device;
            if (!record->content.context)
                entry->request.path = message_attachment_drafts_path(&state->drafts, id);
            if (!media_asset_key_valid(key) || !local_draft_preview_request_valid(entry->request)) {
                // Unsupported formats and oversized sources keep the file card.
                reset_value(&entry->request.snapshot);
                reset_value(&entry->request.content);
                reset_value(&entry->request.path);
                entry->failed = true;
                continue;
            }
        }
        if (entry->ready && g_app.renderer.preview_media_atlas &&
            !media_atlas_peek(g_app.renderer.preview_media_atlas, entry->request.key).ok()) {
            // Evicted under atlas pressure: decode again from the retained source.
            changed();
            entry->ready = false;
            entry->poll_at = 0;
        }
        if (entry->ready || entry->failed || now < entry->poll_at)
            continue;
        // The path view borrows draft payload storage; refresh it per admission.
        if (!entry->request.content.context)
            entry->request.path = message_attachment_drafts_path(&state->drafts, id);
        const auto result = g_app.account.api.media_local_preview(entry->instance, AccountLocalPreviewOperation::request,
                                                                  &entry->request, now, unix_now);
        if (result.state == AccountMediaRequestState::queued) {
            entry->admitted = true;
            ++owner->admissions;
        } else if (result.state == AccountMediaRequestState::pending ||
                   result.state == AccountMediaRequestState::ready) {
            entry->admitted = true;
        } else if (result.state != AccountMediaRequestState::busy) {
            changed();
            entry->failed = true;
            entry->poll_at = 0;
            continue;
        }
        entry->poll_at = deadline_after(now, 100);
    }
    return true;
}

void application_composer_preview_retire(u64 draft_id) noexcept {
    if (owner)
        for (auto& entry : owner->entries)
            if (entry.occupied && !entry.retired && (!draft_id || entry.draft_id == draft_id))
                retire(entry, clock_monotonic_milliseconds());
}

void application_composer_preview_release() noexcept {
    if (!owner)
        return;
    for (auto& entry : owner->entries)
        if (entry.occupied)
            retire(entry, clock_monotonic_milliseconds());
    // Admitted lanes own their retained sources and copied paths.
    secure_clear_bytes(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
    changed();
}

u64 application_composer_preview_next_wake() noexcept {
    u64 wake = 0;
    if (owner)
        for (const auto& entry : owner->entries)
            if (entry.occupied && entry.poll_at && (!wake || entry.poll_at < wake))
                wake = entry.poll_at;
    return wake;
}

u32 application_composer_preview_revision() noexcept {
    return publication;
}

ApplicationComposerPreviewMetrics application_composer_preview_metrics() noexcept {
    ApplicationComposerPreviewMetrics value{};
    if (!owner)
        return value;
    value.admissions = owner->admissions;
    value.retirements = owner->retirements;
    for (const auto& entry : owner->entries)
        if (entry.occupied && !entry.retired) {
            ++value.active;
            value.pending += entry.admitted && !entry.ready && !entry.failed;
            value.ready += entry.ready;
            value.failed += entry.failed;
        }
    return value;
}

} // namespace maicord::app_detail

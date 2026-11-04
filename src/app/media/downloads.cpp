#include "downloads_internal.hpp"
#include "../chat/composer/text_input.hpp"
#include "policy.hpp"
#include "../account/account_menu.hpp"
#include "../account/profile.hpp"
#include "video.hpp"
#include "../settings/settings.hpp"
#include "../plugins/plugins.hpp"
#include "../settings/theme_editor.hpp"
#include "../shell/toast.hpp"
#include "maicord/protocol/media/attachment_download.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"
#include "maicord/app/media/account_media.hpp"

namespace maicord::app_detail {
namespace downloads_detail {

bool account_current(const ApplicationDownloads& q) noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           q.account_ui == g_app.account_ui && q.account_instance == g_app.account.instance &&
           q.scope == g_app.plugin_account_scope_serial && q.profile == g_app.account.status.profile_id &&
           q.self == g_app.account_ui->chat_state.self_user_id;
}

} // namespace downloads_detail
namespace {

bool account_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && g_app.plugin_account_scope_serial && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id && g_app.account.api.download;
}

} // namespace
namespace downloads_detail {

bool foreground_form() noexcept {
    return application_settings_active() || application_theme_editor_active() ||
           (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
            g_app.renderer.forum_form->scene->active) ||
           (g_app.renderer.text_form && g_app.renderer.text_form->scene &&
            g_app.renderer.text_form->scene->active);
}

} // namespace downloads_detail
namespace {

bool channel_current(u64 id) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, id);
    if (!channel || (channel->flags & kChannelObfuscatedFlag) ||
        !channel_is_message_capable(channel->type))
        return false;
    if (!channel->guild_id)
        return channel->type == 1 || channel->type == 3;
    const auto* guild = chat_state_store_find_guild(state, channel->guild_id);
    return guild && !guild->unavailable && channel->permissions_resolved &&
           (channel->actions & channel_action_view) &&
           (channel->actions & channel_action_read_history) &&
           chat_state_store_channel_visible(state, *channel);
}

// The message's rich record, or its forwarded snapshot's.
const MessageRichRecord* rich_of(const MessageRichStore& store, u64 message_id, bool snapshot) noexcept {
    return snapshot ? message_rich_store_snapshot(store, message_id)
                    : message_rich_store_find(store, message_id);
}
// The outer rich record of a message and the one a source reads (its forwarded
// snapshot's when `snapshot`), both current for the message's revision.
bool rich_current(const MessageRichStore& store, const MessageRecord& message, bool snapshot,
                  const MessageRichRecord** outer, const MessageRichRecord** rich) noexcept {
    *outer = message_rich_store_find(store, message.id);
    *rich = snapshot ? message_rich_store_snapshot(store, message.id) : *outer;
    return *outer && *rich && (*rich)->message_revision == message.revision &&
           (*outer)->message_revision == message.revision;
}
// The active channel's message with this ID.
const MessageRecord* active_message(u64 message_id, MessageHandle* handle) noexcept {
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!message || message->channel_id != g_app.account_ui->active_channel_id)
        return nullptr;
    *handle = found.handle;
    return message;
}
// The identity and scope fields every source shares.
void bind_message(DownloadSource* source, const MessageRecord& message) noexcept {
    source->scope = g_app.plugin_account_scope_serial;
    source->profile = g_app.account.status.profile_id;
    source->self = g_app.account_ui->chat_state.self_user_id;
    source->channel = message.channel_id;
    source->message = message.id;
    source->revision = message.revision;
}
bool capture_payload(const MessageRecord& message, MessageHandle handle, const MessageRichStore& store,
                     u64 attachment_id, bool snapshot, DownloadSource* out) noexcept {
    if (!account_ready() || !attachment_id ||
        message.delivery != MessageDeliveryState::authoritative || !channel_current(message.channel_id))
        return false;
    const MessageRichRecord *outer = nullptr, *rich = nullptr;
    if (!rich_current(store, message, snapshot, &outer, &rich) ||
        (snapshot &&
         (outer->reference.type != 1 || !outer->reference.channel_id || !outer->reference.message_id)))
        return false;
    const auto origin = snapshot ? outer->reference.channel_id : message.channel_id;
    for (u32 i = 0; i < rich->attachment_count; ++i) {
        const auto& a = rich->attachments[i];
        if (a.id != attachment_id)
            continue;
        const auto url = message_rich_store_string(store, a.url);
        if (!url.data || !url.bytes || url.bytes >= sizeof(out->url) ||
            !received_attachment_url_current(url, origin, attachment_id))
            return false;
        DownloadSource source{};
        const auto name = attachment_download_safe_filename(message_rich_store_string(store, a.filename),
                                                            {source.filename, sizeof(source.filename)});
        if (!name.ok() || !name.bytes)
            return false;
        bind_message(&source, message);
        source.attachment = attachment_id;
        source.origin_channel = origin;
        source.origin_message = snapshot ? outer->reference.message_id : message.id;
        source.handle = handle;
        source.bytes = a.size;
        source.snapshot = snapshot;
        source.risky = name.risky;
        source.filename_bytes = static_cast<u16>(name.bytes);
        source.url_bytes = static_cast<u16>(url.bytes);
        copy_bytes(source.url, url.data, url.bytes);
        source.fingerprint = hash64_counted_bytes(kHash64Seed, url.data, url.bytes);
        *out = source;
        return true;
    }
    return false;
}
bool capture(u64 message_id, u64 attachment_id, bool snapshot, DownloadSource* out) noexcept {
    if (!account_ready() || !message_id || !attachment_id)
        return false;
    MessageHandle handle{};
    const auto* message = active_message(message_id, &handle);
    return message && capture_payload(*message, handle, g_app.account_ui->message_rich_store, attachment_id,
                                      snapshot, out);
}
bool capture_search(const ApplicationSearchSourceTicket& ticket, u64 attachment_id, bool snapshot,
                    DownloadSource* out) noexcept {
    ApplicationSearchSourceView view{};
    if (!application_search_source_resolve(ticket, snapshot, &view) || !view.message ||
        !view.rich_store ||
        !capture_payload(*view.message, view.handle, *view.rich_store, attachment_id, snapshot, out))
        return false;
    out->search_source = ticket;
    out->search_result = true;
    return true;
}
// The media an embed shows, as the message scene selects it: a gallery tile's
// own media, or a merged embed's host.
bool embed_media(const MessageRichStore& store, const MessageRichRecord& rich, u32 index,
                 MessageRichEmbedMediaSelection* selected) noexcept {
    MessageRichEmbedGalleryPlace place{};
    const bool tile = message_rich_embed_gallery_place(store, rich, index, &place);
    if (!tile && message_rich_embed_merge_host(store, rich, index) != index)
        return false;
    return tile ? message_rich_embed_gallery_select(store, rich.embeds[index], selected)
                : message_rich_embed_media_select(store, rich.embeds[index], selected);
}
bool media_downloadable(const Utf8View& url) noexcept {
    u8 request[kCdnMaximumTargetBytes + 192];
    return url.bytes && attachment_download_write_media_request(url, {request, sizeof(request)}).ok();
}
// The saved name of embed media: the last path segment of its original URL,
// with an extension for its format when the segment has none.
bool embed_filename(Utf8View url, MediaAttachmentFormat format, DownloadSource* out) noexcept {
    u32 start = 0, end = url.bytes, slashes = 0;
    for (u32 i = 0; i < url.bytes; ++i) {
        if (url.data[i] == '?' || url.data[i] == '#') {
            end = i;
            break;
        }
        if (url.data[i] == '/' && ++slashes >= 3)
            start = i + 1;
    }
    const char* extension = format == MediaAttachmentFormat::gif    ? ".gif"
                            : format == MediaAttachmentFormat::mp4  ? ".mp4"
                            : format == MediaAttachmentFormat::png  ? ".png"
                            : format == MediaAttachmentFormat::jpeg ? ".jpg"
                            : format == MediaAttachmentFormat::webp ? ".webp"
                                                                    : "";
    char name[sizeof(out->filename)]{};
    u32 bytes = slashes >= 3 && end > start ? minimum<u32>(end - start, sizeof(name) - 8) : 0;
    copy_bytes(name, url.data + start, bytes);
    if (!bytes) {
        copy_bytes(name, format == MediaAttachmentFormat::mp4 ? "video" : "image", 5);
        bytes = 5;
    }
    bool dotted = false;
    for (u32 i = 0; i < bytes; ++i)
        dotted = dotted || name[i] == '.';
    for (u32 i = 0; !dotted && extension[i]; ++i)
        name[bytes++] = extension[i];
    const auto safe =
        attachment_download_safe_filename({name, bytes}, {out->filename, sizeof(out->filename)});
    if (!safe.ok() || !safe.bytes)
        return false;
    out->filename_bytes = static_cast<u16>(safe.bytes);
    out->risky = safe.risky;
    return true;
}
bool capture_embed_payload(const MessageRecord& record, MessageHandle handle, const MessageRichStore& store,
                           u32 embed_index, bool snapshot, DownloadSource* out) noexcept {
    if (record.delivery != MessageDeliveryState::authoritative ||
        !channel_current(record.channel_id) || embed_index > 0xFF)
        return false;
    const MessageRichRecord *outer = nullptr, *rich = nullptr;
    MessageRichEmbedMediaSelection selected{};
    if (!rich_current(store, record, snapshot, &outer, &rich) || embed_index >= rich->embed_count ||
        !embed_media(store, *rich, embed_index, &selected) || !selected.media)
        return false;
    // The media proxy serves any embed media; a raw third-party URL is never fetched.
    const Utf8View original = message_rich_store_string(store, selected.media->url);
    Utf8View url = message_rich_store_string(store, selected.media->proxy_url);
    if (!media_downloadable(url))
        url = original;
    if (!media_downloadable(url) || url.bytes >= sizeof(out->url))
        return false;
    DownloadSource source{};
    const bool gifv = rich->embeds[embed_index].type == GatewayMessageEmbedType::gifv;
    if (!embed_filename(original.bytes ? original : url,
                        gifv ? MediaAttachmentFormat::mp4 : selected.format, &source))
        return false;
    bind_message(&source, record);
    source.origin_channel = record.channel_id;
    source.origin_message = record.id;
    source.handle = handle;
    source.bytes = kHttpsFileDownloadSizeFromResponse;
    source.snapshot = snapshot;
    source.embed = true;
    source.embed_index = static_cast<u8>(embed_index);
    source.url_bytes = static_cast<u16>(url.bytes);
    copy_bytes(source.url, url.data, url.bytes);
    source.fingerprint = hash64_counted_bytes(kHash64Seed, url.data, url.bytes);
    *out = source;
    return true;
}
bool capture_embed(u64 message_id, u32 embed_index, bool snapshot, DownloadSource* out) noexcept {
    if (!account_ready() || !message_id)
        return false;
    MessageHandle handle{};
    const auto* message = active_message(message_id, &handle);
    return message && capture_embed_payload(*message, handle, g_app.account_ui->message_rich_store,
                                            embed_index, snapshot, out);
}

} // namespace
namespace downloads_detail {

bool source_current(const DownloadSource& s) noexcept {
    if (!account_ready() || s.scope != g_app.plugin_account_scope_serial ||
        s.profile != g_app.account.status.profile_id ||
        s.self != g_app.account_ui->chat_state.self_user_id)
        return false;
    DownloadSource live{};
    const bool valid = s.embed           ? capture_embed(s.message, s.embed_index, s.snapshot, &live)
                       : s.search_result ? capture_search(s.search_source, s.attachment, s.snapshot, &live)
                                         : capture(s.message, s.attachment, s.snapshot, &live);
    return valid && live.handle.slot == s.handle.slot && live.handle.generation == s.handle.generation &&
           live.revision == s.revision && live.channel == s.channel &&
           live.origin_channel == s.origin_channel && live.origin_message == s.origin_message &&
           live.bytes == s.bytes && live.fingerprint == s.fingerprint && live.url_bytes == s.url_bytes &&
           bytes_equal(live.url, s.url, s.url_bytes) && live.filename_bytes == s.filename_bytes &&
           bytes_equal(live.filename, s.filename, s.filename_bytes);
}
bool source_media_allowed(const DownloadSource& source, bool activation) noexcept {
    const MessageRecord* message{};
    const MessageRichStore* store{};
    const MessageRichRecord* outer{};
    const MessageRichRecord* rich{};
    MessageScene* scene{};
    if (source.search_result) {
        ApplicationSearchSourceView view{}, child{};
        if (!application_search_source_resolve(source.search_source, false, &view) ||
            (source.snapshot &&
             !application_search_source_resolve(source.search_source, true, &child)))
            return false;
        message = view.message;
        store = view.rich_store;
        outer = view.rich;
        rich = source.snapshot ? child.rich : outer;
        scene = ui_search_scene_message_scene(application_search_scene());
    } else {
        message = message_store_resolve(g_app.message_view.store, source.handle);
        store = &g_app.account_ui->message_rich_store;
        outer = message_rich_store_find(*store, source.message);
        rich = source.snapshot ? message_rich_store_snapshot(*store, source.message) : outer;
        scene = &g_app.message_scene;
    }
    if (!message || !store || !outer || !rich || !scene || message->id != source.message ||
        message->revision != source.revision || outer->message_revision != source.revision ||
        rich->message_revision != source.revision)
        return false;
    u32 index = source.embed_index;
    bool spoiler = false, explicit_media = false;
    if (source.embed) {
        if (index >= rich->embed_count)
            return false;
        explicit_media = message_rich_embed_explicit(rich->embeds[index]);
    } else {
        index = 0;
        while (index < rich->attachment_count && rich->attachments[index].id != source.attachment)
            ++index;
        if (index == rich->attachment_count)
            return false;
        spoiler = message_rich_attachment_spoiler(*store, rich->attachments[index]);
        explicit_media = message_rich_attachment_explicit(rich->attachments[index]);
    }
    const auto owner = message_media_owner(*message, outer);
    const auto key = message_media_reveal_key(
        source.message, source.embed ? MessageMediaSource::embed : MessageMediaSource::attachment, index,
        source.snapshot);
    // Only a user activation may reveal or request age confirmation. Pending
    // admission rechecks policy without creating either side effect.
    return activation ? !application_media_activation_consumed(*scene, owner, source.message, key, spoiler,
                                                               explicit_media)
                      : message_scene_media_obscure(*scene, owner, source.message, key, spoiler,
                                                    explicit_media) == MessageMediaObscure::none;
}
void changed(ApplicationDownloads& q) noexcept {
    ++q.revision;
    if (!q.revision)
        ++q.revision;
    if (q.active)
        application_request_frame();
}
void release_path(DownloadJob& j) noexcept {
    if (j.path) {
        SecureZeroMemory(j.path, (j.path_units + 1) * sizeof(wchar_t));
        pages_release(j.path);
    }
    j.path = nullptr;
    j.path_units = 0;
    j.chosen_name_bytes = 0;
}

} // namespace downloads_detail
namespace {

void retire_job(DownloadJob& j) noexcept {
    release_path(j);
    SecureZeroMemory(&j, sizeof(j));
}

} // namespace
namespace downloads_detail {

ApplicationDownloads* ensure_owner() noexcept {
    if (g_app.downloads)
        return account_current(*g_app.downloads) && !g_app.downloads->closing ? g_app.downloads : nullptr;
    if (!account_ready())
        return nullptr;
    auto* q = static_cast<ApplicationDownloads*>(
        commit_pages(sizeof(ApplicationDownloads)));
    if (!q)
        return nullptr;
    q->account_ui = g_app.account_ui;
    q->account_instance = g_app.account.instance;
    q->scope = g_app.plugin_account_scope_serial;
    q->profile = g_app.account.status.profile_id;
    q->self = g_app.account_ui->chat_state.self_user_id;
    q->active_job = q->picker_job = kNoDownload;
    q->revision = 1;
    g_app.downloads = q;
    return q;
}

} // namespace downloads_detail
namespace {

DownloadJob* find_job(ApplicationDownloads& q, u64 id, u32* index = nullptr) noexcept {
    for (u32 i = 0; i < kDownloadJobs; ++i)
        if (q.jobs[i].occupied && q.jobs[i].id == id) {
            if (index)
                *index = i;
            return &q.jobs[i];
        }
    return nullptr;
}

} // namespace
namespace downloads_detail {

bool terminal(JobPhase p) noexcept {
    return p == JobPhase::saved || p == JobPhase::failed || p == JobPhase::cancelled;
}

} // namespace downloads_detail
namespace {

Utf8View job_status(const DownloadJob& j) noexcept {
    switch (j.phase) {
    case JobPhase::warning:
        return utf8_literal("Confirmation required");
    case JobPhase::queued:
        return utf8_literal("Queued");
    case JobPhase::choosing:
        return utf8_literal("Choose where to save");
    case JobPhase::resolving:
        return utf8_literal("Refreshing link…");
    case JobPhase::transferring:
        if (j.transfer.phase == HttpsFileDownloadPhase::complete) {
            if (j.transfer.cleanup_pending)
                return utf8_literal("Save failed");
            if (j.transfer.ok())
                return utf8_literal("Saved");
            if (j.transfer.error == HttpsFileDownloadError::cancelled)
                return utf8_literal("Cancelled");
            return utf8_literal("Save failed");
        }
        return j.cancel_requested                                       ? utf8_literal("Cancelling…")
               : j.transfer.phase == HttpsFileDownloadPhase::committing ? utf8_literal("Finishing…")
                                                                        : utf8_literal("Downloading");
    case JobPhase::saved:
        return utf8_literal("Saved");
    case JobPhase::cancelled:
        return utf8_literal("Cancelled");
    case JobPhase::failed:
        if (j.transfer.error == HttpsFileDownloadError::destination_changed)
            return utf8_literal("The save location changed. Try again.");
        if (j.transfer.error == HttpsFileDownloadError::size_mismatch)
            return utf8_literal("Download incomplete. Nothing was saved.");
        if (j.transfer.error == HttpsFileDownloadError::invalid_input)
            return utf8_literal("The attachment changed. Save it again from the message.");
        if (j.transfer.status_code == 403 || j.transfer.status_code == 404)
            return utf8_literal("Link expired. Try again.");
        if (j.transfer.cleanup_pending)
            return utf8_literal("Save failed");
        return utf8_literal("Save failed. Try again.");
    }
    return {};
}
// The name the save will use: the picked one, else the attachment's.
Utf8View job_name(const DownloadJob& j) noexcept {
    return j.chosen_name_bytes ? Utf8View{j.chosen_name, j.chosen_name_bytes}
                               : Utf8View{j.source.filename, j.source.filename_bytes};
}

} // namespace
namespace downloads_detail {

bool scene_current(void* opaque) noexcept {
    const auto* q = static_cast<const ApplicationDownloads*>(opaque);
    return q && q == g_app.downloads && q->active && account_current(*q) && !q->closing &&
           !foreground_form();
}

} // namespace downloads_detail
namespace {

bool resolve_row(void* opaque, u32 index, UiDownloadRow* out) noexcept {
    auto* q = static_cast<ApplicationDownloads*>(opaque);
    if (!out || !scene_current(q))
        return false;
    for (auto& j : q->jobs)
        if (j.occupied) {
            if (index--)
                continue;
            const bool transfer_complete =
                j.admitted && j.transfer.phase == HttpsFileDownloadPhase::complete;
            const bool done = terminal(j.phase) || transfer_complete;
            const auto phase = j.phase == JobPhase::saved       ? UiDownloadPhase::saved
                               : j.phase == JobPhase::failed    ? UiDownloadPhase::failed
                               : j.phase == JobPhase::cancelled ? UiDownloadPhase::cancelled
                               : j.phase == JobPhase::transferring
                                   ? (j.transfer.phase == HttpsFileDownloadPhase::committing
                                          ? UiDownloadPhase::committing
                                          : UiDownloadPhase::downloading)
                               : j.phase == JobPhase::choosing || j.phase == JobPhase::resolving
                                   ? UiDownloadPhase::preparing
                                   : UiDownloadPhase::queued;
            const auto shown_phase =
                transfer_complete
                    ? (j.transfer.ok()                                         ? UiDownloadPhase::saved
                       : j.transfer.error == HttpsFileDownloadError::cancelled ? UiDownloadPhase::cancelled
                                                                               : UiDownloadPhase::failed)
                    : phase;
            *out = {j.id,
                    job_name(j),
                    j.source.embed           ? utf8_literal("Embedded media")
                    : j.source.snapshot      ? utf8_literal("Forwarded attachment")
                    : j.source.search_result ? utf8_literal("Search attachment")
                                             : utf8_literal("Message attachment"),
                    job_status(j), j.transfer.committed_bytes,
                    // Embed media learns its length from the response.
                    j.source.embed ? j.transfer.file_bytes : j.source.bytes, shown_phase,
                    j.phase == JobPhase::choosing || j.phase == JobPhase::resolving,
                    !done && !j.cancel_requested,
                    (j.phase == JobPhase::failed || j.phase == JobPhase::cancelled) && !j.admitted &&
                        q->picker_job != static_cast<u32>(&j - q->jobs) && source_current(j.source),
                    j.phase == JobPhase::saved && j.path,
                    done && !j.admitted && q->picker_job != static_cast<u32>(&j - q->jobs)};
            return true;
        }
    return false;
}

} // namespace
namespace downloads_detail {

void revoke_capture(ApplicationDownloads& q) noexcept {
    const bool owned = q.captured || q.scene.widgets.captured.generation;
    g_app.downloads_pointer_tail = g_app.downloads_pointer_tail || q.captured;
    q.captured = false;
    reset_value(&q.pressed);
    reset_value(&q.pressed_widget);
    reset_value(&q.pressed_bounds);
    ui_widget_manager_capture(&q.scene.widgets, ui_widget_invalid_handle());
    if (owned)
        application_release_capture();
}
void page_step(ApplicationDownloads& q, bool previous) noexcept {
    revoke_capture(q);
    ui_download_scene_page(&q.scene, previous ? (q.scene.page ? q.scene.page - 1 : 0) : q.scene.page + 1);
}
// Why a save failed, without its path or URL.
void log_failure(const HttpsFileDownloadResult& transfer) noexcept {
    log_u64(LogLevel::warning, "download", "failed_error", static_cast<u64>(transfer.error));
    if (transfer.native_error)
        log_u64(LogLevel::warning, "download", "failed_native_error", transfer.native_error);
    if (transfer.status_code)
        log_u64(LogLevel::warning, "download", "failed_status", transfer.status_code);
}

} // namespace downloads_detail
namespace {

// Pictures confirm as images, by the saved name's extension.
bool image_file(const DownloadJob& j) noexcept {
    const Utf8View name = job_name(j);
    static constexpr Utf8View kImages[]{utf8_literal(".png"), utf8_literal(".jpg"), utf8_literal(".jpeg"),
                                 utf8_literal(".gif"), utf8_literal(".webp"), utf8_literal(".avif")};
    for (const auto extension : kImages)
        if (name.bytes > extension.bytes &&
            ascii_equal_ignore_case({name.data + name.bytes - extension.bytes, extension.bytes}, extension))
            return true;
    return false;
}

} // namespace
namespace downloads_detail {

void toast_result(const DownloadJob& j) noexcept {
    const bool image = image_file(j);
    application_toast_result(j.phase == JobPhase::saved, image ? kToastImageSaved : kToastAttachmentSaved,
                             image ? kToastImageSaveFailed : kToastAttachmentSaveFailed);
}
void fail_job(ApplicationDownloads& q, DownloadJob& j, HttpsFileDownloadError error) noexcept {
    j.phase = JobPhase::failed;
    j.transfer.error = error;
    reset_value(&j.resolution);
    log_failure(j.transfer);
    q.reveal = true;
    toast_result(j);
    changed(q);
}
void cancel_job(ApplicationDownloads& q, u32 index) noexcept {
    auto& j = q.jobs[index];
    if (terminal(j.phase))
        return;
    j.cancel_requested = true;
    if (q.picker_job == index)
        native_file_picker_cancel(&q.picker);
    if (j.admitted && q.account_instance == g_app.account.instance && g_app.account.api.download)
        g_app.account.api.download(q.account_instance, AccountDownloadOperation::cancel, j.id, nullptr);
    else if (q.picker_job != index) {
        j.phase = JobPhase::cancelled;
        reset_value(&j.resolution);
    }
    if (q.warning_id == j.id)
        q.warning_id = 0;
    changed(q);
}
bool begin_source(const DownloadSource& source) noexcept {
    if (!source_current(source) || !source_media_allowed(source, true))
        return true;
    auto* q = ensure_owner();
    if (!q)
        return false;
    u32 slot = kNoDownload;
    for (u32 i = 0; i < kDownloadJobs; ++i) {
        const auto& j = q->jobs[i];
        if (!j.occupied) {
            if (slot == kNoDownload)
                slot = i;
            continue;
        }
        const bool same_search =
            j.source.search_result == source.search_result &&
            (!source.search_result ||
             (j.source.search_source.publication_revision == source.search_source.publication_revision &&
              j.source.search_source.index == source.search_source.index));
        if (!terminal(j.phase) && same_search && j.source.message == source.message &&
            j.source.attachment == source.attachment && j.source.snapshot == source.snapshot &&
            j.source.embed == source.embed && j.source.embed_index == source.embed_index &&
            j.source.fingerprint == source.fingerprint && (j.admitted || source_current(j.source)))
            return application_downloads_open();
    }
    // Saves finish without the panel, so their receipts pile up unseen: a new
    // save reuses the oldest finished one rather than asking for a dismissal.
    if (slot == kNoDownload)
        for (u32 i = 0; i < kDownloadJobs; ++i) {
            const auto& j = q->jobs[i];
            if (terminal(j.phase) && !j.admitted && q->picker_job != i &&
                (slot == kNoDownload || j.id < q->jobs[slot].id))
                slot = i;
        }
    if (slot == kNoDownload)
        return application_downloads_open();
    auto& j = q->jobs[slot];
    retire_job(j);
    j.occupied = true;
    j.source = source;
    j.id = ++g_app.downloads_lifetime_serial;
    j.phase = source.risky ? JobPhase::warning : JobPhase::queued;
    j.approved = !source.risky;
    if (source.risky && !q->warning_id)
        q->warning_id = j.id;
    changed(*q);
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    // Like the official client, saving goes straight to the save dialog; only
    // a risky file type first asks for confirmation in the panel.
    return !source.risky || application_downloads_open();
}

} // namespace downloads_detail

bool application_downloads_available(u64 message, u64 attachment, bool snapshot) noexcept {
    DownloadSource source{};
    return capture(message, attachment, snapshot, &source);
}
bool application_downloads_begin(u64 message, u64 attachment, bool snapshot) noexcept {
    DownloadSource source{};
    return !capture(message, attachment, snapshot, &source) || begin_source(source);
}
bool application_downloads_search_available(const ApplicationSearchSourceTicket& ticket, u64 attachment,
                                            bool snapshot) noexcept {
    DownloadSource source{};
    return capture_search(ticket, attachment, snapshot, &source);
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& ticket, u64 attachment,
                                        bool snapshot) noexcept {
    DownloadSource source{};
    return !capture_search(ticket, attachment, snapshot, &source) || begin_source(source);
}
bool application_downloads_embed_available(u64 message, u32 embed, bool snapshot) noexcept {
    DownloadSource source{};
    return capture_embed(message, embed, snapshot, &source);
}
bool application_downloads_begin_embed(u64 message, u32 embed, bool snapshot) noexcept {
    DownloadSource source{};
    return !capture_embed(message, embed, snapshot, &source) || begin_source(source);
}
bool application_downloads_active() noexcept {
    return g_app.downloads && g_app.downloads->active;
}
void application_downloads_close() noexcept {
    auto* q = g_app.downloads;
    if (!q || !q->active)
        return;
    revoke_capture(*q);
    q->active = false;
    q->scene.active = false;
    q->scene.source = nullptr;
    q->scene.content = nullptr;
    g_app.renderer.download_scene = nullptr;
    if (g_app.context_focus == &q->scene.widgets)
        g_app.context_focus = nullptr;
    if (g_app.renderer.focus_owner == &q->scene.widgets)
        renderer_bind_focus_owner(&g_app.renderer, nullptr);
    application_surface_motion_retire(RendererLayerSurface::popup);
    application_request_frame();
}
bool application_downloads_open() noexcept {
    if (foreground_form())
        return true;
    auto* q = ensure_owner();
    if (!q || g_app.login.visible || !g_app.renderer_ready)
        return true;
    application_composer_text_input_suspend();
    application_message_forward_close();
    application_quick_switcher_close();
    application_account_menu_clear();
    application_image_viewer_close();
    application_video_blur();
    // Search remains the accepted source beneath this modal overlay. Explicit
    // Search close/query/page actions, rather than opening Downloads, retire it.
    application_pinned_messages_close();
    application_media_picker_close();
    if (!application_profile_close() || !dismiss_popup())
        return false;
    q->active = true;
    if (g_app.window)
        SetFocus(g_app.window);
    application_release_capture();
    return application_downloads_synchronize() && application_request_frame();
}
bool application_downloads_synchronize() noexcept {
    auto* q = g_app.downloads;
    if (!q || !q->active)
        return true;
    if (!account_current(*q) || q->closing || g_app.login.visible || foreground_form()) {
        application_downloads_close();
        return true;
    }
    u32 count = 0;
    for (const auto& j : q->jobs)
        count += j.occupied;
    q->source = {q, count, q->revision, resolve_row, scene_current};
    reset_value(&q->content);
    q->content.heading = utf8_literal("Downloads");
    q->content.revision = q->revision;
    q->content.status = count == kDownloadJobs
                            ? utf8_literal("Dismiss a finished download to save another file.")
                        : count ? Utf8View{}
                                : utf8_literal("No downloads yet");
    if (q->warning_id) {
        auto* j = find_job(*q, q->warning_id);
        if (j && j->phase == JobPhase::warning) {
            q->content.mode = UiDownloadMode::confirmation;
            q->content.confirmation_id = j->id;
            q->content.filename = job_name(*j);
            q->content.warning =
                utf8_literal("This file type can harm your computer. Only save it if you trust the sender.");
        } else
            q->warning_id = 0;
    }
    if (!ui_download_scene_update(&q->scene, &q->source, &q->content, application_viewport_dips()))
        return false;
    g_app.renderer.download_scene = &q->scene;
    g_app.context_focus = &q->scene.widgets;
    return true;
}

namespace {

bool chosen_risky(ConstSlice<wchar_t> path) noexcept {
    u32 start = 0;
    for (u32 i = 0; i < path.count; ++i)
        if (path.data[i] == L'\\' || path.data[i] == L'/')
            start = i + 1;
    start = maximum(start, path.count > 128 ? path.count - 128 : 0u);
    char tail[513]{};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data + start,
                            static_cast<int>(path.count - start), tail, sizeof(tail) - 1, nullptr, nullptr);
    return bytes <= 0 || attachment_download_risk_extension({tail, static_cast<u32>(bytes)});
}

} // namespace
namespace downloads_detail {

void picker_complete(ApplicationDownloads& q) noexcept {
    if (q.picker_job == kNoDownload)
        return;
    auto& j = q.jobs[q.picker_job];
    const auto result = native_file_picker_status(&q.picker);
    if (result.phase != NativeFilePickerPhase::complete)
        return;
    if (j.phase == JobPhase::choosing) {
        if (j.cancel_requested || result.error == NativeFilePickerError::cancelled || q.closing)
            j.phase = JobPhase::cancelled;
        else if (!source_current(j.source) || !source_media_allowed(j.source))
            fail_job(q, j, HttpsFileDownloadError::invalid_input);
        else {
            NativeFileSaveSelection selected{};
            if (!result.ok() || !native_file_picker_save_selection(&q.picker, &selected))
                fail_job(q, j, HttpsFileDownloadError::file_open);
            else {
                const auto units = selected.path.count;
                j.path = static_cast<wchar_t*>(commit_pages((units + 1) * sizeof(wchar_t)));
                if (!j.path)
                    fail_job(q, j, HttpsFileDownloadError::memory);
                else {
                    j.path_units = units;
                    copy_bytes(j.path, selected.path.data, units * sizeof(wchar_t));
                    j.destination = selected.destination;
                    j.destination_exists = selected.destination_exists;
                    j.overwrite_authorized = selected.overwrite_authorized;
                    u32 basename = 0;
                    for (u32 n = 0; n < units; ++n)
                        if (j.path[n] == L'\\' || j.path[n] == L'/')
                            basename = n + 1;
                    char name[2048]{};
                    const int name_bytes = WideCharToMultiByte(
                        CP_UTF8, WC_ERR_INVALID_CHARS, j.path + basename,
                        static_cast<int>(units - basename), name, sizeof(name), nullptr, nullptr);
                    if (name_bytes > 0) {
                        const auto safe = attachment_download_safe_filename(
                            {name, static_cast<u32>(name_bytes)}, {j.chosen_name, sizeof(j.chosen_name)});
                        if (safe.ok())
                            j.chosen_name_bytes = static_cast<u16>(safe.bytes);
                    }
                    j.after_choice_warning = chosen_risky(selected.path) && !j.source.risky;
                    j.phase = j.after_choice_warning ? JobPhase::warning : JobPhase::resolving;
                    if (j.after_choice_warning) {
                        j.approved = false;
                        q.warning_id = j.id;
                        q.reveal = true;
                    }
                }
            }
        }
        changed(q);
    }
    if (native_file_picker_release(&q.picker)) {
        q.picker_job = kNoDownload;
        changed(q);
    }
}
void activate(ApplicationDownloads& q, UiDownloadTarget target) noexcept {
    if (!scene_current(&q))
        return;
    if (target.action == UiDownloadAction::close) {
        application_downloads_close();
        return;
    }
    if (target.action == UiDownloadAction::previous || target.action == UiDownloadAction::next) {
        page_step(q, target.action == UiDownloadAction::previous);
        application_request_frame();
        return;
    }
    u32 index = 0;
    auto* j = find_job(q, target.id, &index);
    if (!j)
        return;
    if (target.action == UiDownloadAction::cancel || target.action == UiDownloadAction::cancel_confirmation)
        cancel_job(q, index);
    else if (target.action == UiDownloadAction::confirm && q.warning_id == j->id &&
             j->phase == JobPhase::warning) {
        const bool current = source_current(j->source);
        if (current && !source_media_allowed(j->source, true))
            return;
        if (!current)
            fail_job(q, *j, HttpsFileDownloadError::invalid_input);
        else {
            j->approved = true;
            j->phase = j->after_choice_warning ? JobPhase::resolving : JobPhase::queued;
        }
        q.warning_id = 0;
        changed(q);
    } else if (target.action == UiDownloadAction::retry && terminal(j->phase) &&
               j->phase != JobPhase::saved && !j->admitted && q.picker_job != index &&
               source_current(j->source)) {
        if (!source_media_allowed(j->source, true))
            return;
        j->force_refresh =
            j->force_refresh || j->transfer.status_code == 403 || j->transfer.status_code == 404;
        release_path(*j);
        reset_value(&j->transfer);
        reset_value(&j->resolution);
        j->cancel_requested = false;
        j->after_choice_warning = false;
        j->phase = j->source.risky ? JobPhase::warning : JobPhase::queued;
        j->approved = !j->source.risky;
        if (j->source.risky)
            q.warning_id = j->id;
        changed(q);
    } else if (target.action == UiDownloadAction::dismiss && terminal(j->phase) && !j->admitted &&
               q.picker_job != index) {
        retire_job(*j);
        changed(q);
    } else if (target.action == UiDownloadAction::reveal && j->phase == JobPhase::saved && j->path) {
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(j->path);
        if (item) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        }
    }
    application_downloads_synchronize();
}

} // namespace downloads_detail
namespace {

struct SceneRich {
    const MessageSceneRowWidget* row;
    const MessageRecord* message;
    const MessageRichRecord* rich;
};
// The row of the message scene (or its forwarded snapshot's) with a current rich record.
bool scene_rich(const MessageScene& scene, u32 r, u32 s, bool active_channel, SceneRich* out) noexcept {
    out->row = s ? message_scene_snapshot_row(scene, scene.rows[r]) : &scene.rows[r];
    out->message = out->row ? message_store_resolve(g_app.message_view.store, out->row->message) : nullptr;
    out->rich = out->message ? rich_of(g_app.account_ui->message_rich_store, out->message->id, s != 0)
                             : nullptr;
    return out->rich && out->rich->message_revision == out->message->revision &&
           (!active_channel || out->message->channel_id == g_app.account_ui->active_channel_id);
}
UiRect visible_rect(const UiRect& rectangle, const UiRect& node_clip, const UiRect& row,
                    const UiRect& viewport) noexcept {
    return ui_rect_intersection(rectangle, ui_rect_intersection(node_clip, ui_rect_intersection(row, viewport)));
}

} // namespace
namespace downloads_detail {

bool attachment_at(float x, float y, bool save_only, DownloadSource* out, UiWidgetHandle focused,
                   UiRect* bounds, UiWidgetHandle* handle) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active)
        return false;
    const auto& scene = g_app.message_scene;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 r = 0; r < scene.count; ++r)
        for (u32 s = 0; s < 2; ++s) {
            SceneRich found{};
            if (!scene_rich(scene, r, s, false, &found))
                continue;
            const auto* row = found.row;
            const auto* message = found.message;
            const auto* rich = found.rich;
            for (u32 a = 0; a < rich->attachment_count; ++a) {
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, a);
                if (focused.generation) {
                    if (ui_widget_handle_equal(widget, focused) &&
                        ui_widget_manager_input_enabled(scene.widgets, widget))
                        return capture(message->id, rich->attachments[a].id, s != 0, out);
                    continue;
                }
                const auto* node = message_scene_rich_item_layout_node(
                    scene, *row, MessageSceneRichItemKind::attachment, a);
                const auto* row_node = message_scene_layout_node(scene, row->widget);
                if (!node || !row_node)
                    continue;
                const auto& attachment = rich->attachments[a];
                if (message_rich_attachment_audio_supported(store, attachment)) {
                    // Inline controls own their Download target and button capture.
                    // Context menus still cover the complete responsive card.
                    if (save_only)
                        continue;
                    UiAudioCardGeometry card{};
                    if (!ui_audio_card_geometry(node->content_rectangle, kDefaultUiAudioControlsStyle,
                                                &card, message_rich_record_voice(*rich)))
                        continue;
                    const auto rectangle =
                        visible_rect(card.bounds, node->clip_rectangle, row_node->rectangle, scene.viewport);
                    if (ui_widget_manager_input_enabled(scene.widgets, widget) &&
                        ui_rect_contains(rectangle, x, y) &&
                        capture(message->id, attachment.id, s != 0, out)) {
                        if (bounds)
                            *bounds = rectangle;
                        if (handle)
                            *handle = widget;
                        return true;
                    }
                    continue;
                }
                float width = 0, height = 0;
                const bool file = !message_rich_attachment_preview_extent(
                    store, attachment, ui_rect_width(node->content_rectangle), &width, &height);
                UiRect rectangle = node->rectangle;
                if (file) {
                    rectangle.right = minimum(rectangle.right, rectangle.left + 420.f);
                    rectangle.bottom = rectangle.top + 36.f;
                } else {
                    rectangle.right = rectangle.left + width;
                    rectangle.bottom = rectangle.top + height;
                }
                if (save_only)
                    rectangle = ui_attachment_save_rectangle(rectangle, file);
                if (!ui_widget_manager_input_enabled(scene.widgets, widget))
                    continue;
                rectangle = visible_rect(rectangle, node->clip_rectangle, row_node->rectangle, scene.viewport);
                if (ui_rect_contains(rectangle, x, y) && capture(message->id, attachment.id, s != 0, out)) {
                    if (bounds)
                        *bounds = rectangle;
                    if (handle)
                        *handle = widget;
                    return true;
                }
            }
        }
    return false;
}

} // namespace downloads_detail
namespace {

// The menu row group of attachment media: its format's kind, a voice message
// among the audio.
ReceivedAttachmentKind source_kind(MediaAttachmentFormat format, const MessageRichRecord* rich) noexcept {
    switch (format) {
    case MediaAttachmentFormat::png:
    case MediaAttachmentFormat::jpeg:
    case MediaAttachmentFormat::webp:
        return ReceivedAttachmentKind::image;
    case MediaAttachmentFormat::gif:
        return ReceivedAttachmentKind::animated_image;
    case MediaAttachmentFormat::mp4:
        return ReceivedAttachmentKind::video;
    default:
        if (media_attachment_format_audio_supported(format))
            return rich && message_rich_record_voice(*rich) ? ReceivedAttachmentKind::voice_message
                                                            : ReceivedAttachmentKind::audio;
        return ReceivedAttachmentKind::file;
    }
}

} // namespace
namespace downloads_detail {

// The official client's attachment groups merged into the message menu, before Copy ID.
bool open_context(const DownloadSource& source, const UiRect& anchor) noexcept {
    auto* q = ensure_owner();
    if (!q || !source_current(source) || !dismiss_popup())
        return false;
    q->menu_valid = false;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = rich_of(store, source.message, source.snapshot);
    u32 index = 0;
    while (rich && index < rich->attachment_count && rich->attachments[index].id != source.attachment)
        ++index;
    const bool attachment = rich && index < rich->attachment_count;
    const auto format = attachment ? message_rich_attachment_format(store, rich->attachments[index])
                                   : MediaAttachmentFormat::unknown;
    const auto kind = source_kind(format, rich);
    const ReceivedAttachmentMenuFacts facts{
        kind, {source.url, source.url_bytes}, kind == ReceivedAttachmentKind::image, true};
    const u32 count = received_attachment_menu(facts, {q->menu_items, kReceivedAttachmentMenuItems});
    // Plugins see an image's index in the message, its URL, filename, id, type and size.
    const bool image = attachment && (kind == ReceivedAttachmentKind::image ||
                                      kind == ReceivedAttachmentKind::animated_image);
    const Utf8View content_type = format == MediaAttachmentFormat::png    ? utf8_literal("image/png")
                                  : format == MediaAttachmentFormat::jpeg ? utf8_literal("image/jpeg")
                                  : format == MediaAttachmentFormat::webp ? utf8_literal("image/webp")
                                  : format == MediaAttachmentFormat::gif  ? utf8_literal("image/gif")
                                                                          : Utf8View{};
    const ApplicationPluginMenuExtra plugin{PluginMenuKind::image, index, 0, false, {source.url, source.url_bytes},
                                            {source.filename, source.filename_bytes}, source.attachment, content_type,
                                            image ? static_cast<u32>(rich->attachments[index].width) : 0u,
                                            image ? static_cast<u32>(rich->attachments[index].height) : 0u};
    if (!count || !open_message_media_context_popup(source.message, anchor, {q->menu_items, count},
                                                    image ? &plugin : nullptr))
        return false;
    q->menu = source;
    q->menu_kind = kind;
    q->menu_format = format;
    q->menu_opens = g_app.message_context.opens;
    q->menu_valid = true;
    return application_request_frame();
}
// A message sticker under the pointer (or the focused sticker widget) and
// the image URL the official client's link rows name. Lottie stickers paint
// on a canvas there, so they offer no link rows.
bool sticker_at(float x, float y, UiWidgetHandle focused, DownloadSource* out) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active)
        return false;
    const auto& scene = g_app.message_scene;
    for (u32 r = 0; r < scene.count; ++r)
        for (u32 s = 0; s < 2; ++s) {
            SceneRich found{};
            if (!scene_rich(scene, r, s, true, &found))
                continue;
            const auto* row = found.row;
            const auto* message = found.message;
            const auto* rich = found.rich;
            const auto* row_node = message_scene_layout_node(scene, row->widget);
            for (u32 i = 0; row_node && i < rich->sticker_count; ++i) {
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::sticker, i);
                const auto* node =
                    message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::sticker, i);
                if (!node || !ui_widget_manager_input_enabled(scene.widgets, widget))
                    continue;
                const UiRect visible =
                    visible_rect(node->rectangle, node->clip_rectangle, row_node->rectangle, scene.viewport);
                const bool hit = focused.generation ? ui_widget_handle_equal(widget, focused)
                                                    : ui_rect_contains(visible, x, y);
                if (!hit)
                    continue;
                const auto& sticker = rich->stickers[i];
                if (sticker.format_type == 3)
                    return false;
                u8 target[64];
                const auto written =
                    write_sticker_cdn_target(sticker.id, sticker.format_type, {target, sizeof(target)});
                if (!written.ok())
                    return false;
                // Message sticker images are served from the media proxy.
                DownloadSource source{};
                ProtocolWriter writer{{reinterpret_cast<u8*>(source.url), sizeof(source.url)}, 0};
                if (!protocol_writer_append_literal(&writer, "https://media.discordapp.net") ||
                    !protocol_writer_append_bytes(&writer, target, written.bytes))
                    return false;
                source.url_bytes = static_cast<u16>(writer.used);
                bind_message(&source, *message);
                *out = source;
                return true;
            }
        }
    return false;
}

} // namespace downloads_detail
namespace {

// A right-clicked embed image (each gallery tile on its own) adds the link
// pair for that image's original URL, as a sticker image does.
bool embed_image_at(float x, float y, DownloadSource* out, u32* embed_index = nullptr,
                    bool* snapshot = nullptr) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active || !message_rich_display().embeds)
        return false;
    const auto& scene = g_app.message_scene;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 r = 0; r < scene.count; ++r)
        for (u32 s = 0; s < 2; ++s) {
            SceneRich found{};
            if (!scene_rich(scene, r, s, true, &found))
                continue;
            const auto* row = found.row;
            const auto* message = found.message;
            const auto* rich = found.rich;
            const auto* row_node = message_scene_layout_node(scene, row->widget);
            if (!row_node)
                continue;
            for (u32 i = 0; i < rich->embed_count; ++i) {
                MessageRichEmbedGalleryPlace place{};
                const bool tile = message_rich_embed_gallery_place(store, *rich, i, &place);
                const u32 host = tile ? place.host : i;
                if (!tile && message_rich_embed_merge_host(store, *rich, i) != i)
                    continue;
                const auto* node =
                    message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::embed, host);
                MessageRichEmbedMediaSelection selected{};
                if (!node || !(tile ? message_rich_embed_gallery_select(store, rich->embeds[i], &selected)
                                    : message_rich_embed_media_select(store, rich->embeds[i], &selected)))
                    continue;
                UiRect preview = message_rich_embed_media_rect(
                    row->rich_item_layout, host, node->content_rectangle,
                    message_rich_embed_media_only(rich->embeds[host]));
                if (tile)
                    preview = message_rich_embed_gallery_tile(preview, place.count, place.ordinal);
                const UiRect visible =
                    visible_rect(preview, node->clip_rectangle, row_node->rectangle, scene.viewport);
                if (!ui_rect_contains(visible, x, y))
                    continue;
                Utf8View url = message_rich_store_string(store, selected.media->url);
                if (!url.bytes)
                    url = selected.source;
                if (!url.bytes || url.bytes > sizeof(out->url))
                    return false;
                DownloadSource source{};
                copy_bytes(source.url, url.data, url.bytes);
                source.url_bytes = static_cast<u16>(url.bytes);
                bind_message(&source, *message);
                *out = source;
                if (embed_index)
                    *embed_index = i;
                if (snapshot)
                    *snapshot = s != 0;
                return true;
            }
        }
    return false;
}

} // namespace
namespace downloads_detail {

bool open_sticker_context(const DownloadSource& source, const UiRect& anchor) noexcept {
    auto* q = ensure_owner();
    if (!q || !dismiss_popup())
        return false;
    q->menu_valid = false;
    const ReceivedAttachmentMenuFacts facts{
        ReceivedAttachmentKind::sticker, {source.url, source.url_bytes}, false, false};
    const u32 count = received_attachment_menu(facts, {q->menu_items, kReceivedAttachmentMenuItems});
    if (!count || !open_message_media_context_popup(source.message, anchor, {q->menu_items, count}))
        return false;
    q->menu = source;
    q->menu_kind = ReceivedAttachmentKind::sticker;
    q->menu_opens = g_app.message_context.opens;
    q->menu_valid = true;
    return application_request_frame();
}

} // namespace downloads_detail
namespace {

// What embed media is, for its menu labels: an animation, a video or an image.
ReceivedAttachmentKind embed_kind(const DownloadSource& source) noexcept {
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = rich_of(store, source.message, source.snapshot);
    MessageRichEmbedMediaSelection selected{};
    if (!rich || source.embed_index >= rich->embed_count ||
        !embed_media(store, *rich, source.embed_index, &selected))
        return ReceivedAttachmentKind::file;
    if (rich->embeds[source.embed_index].type == GatewayMessageEmbedType::gifv ||
        selected.format == MediaAttachmentFormat::gif)
        return ReceivedAttachmentKind::animated_image;
    if (selected.format == MediaAttachmentFormat::mp4)
        return ReceivedAttachmentKind::video;
    return selected.format == MediaAttachmentFormat::unknown ? ReceivedAttachmentKind::file
                                                             : ReceivedAttachmentKind::image;
}
// Embed media gets Save beside its link rows, which name the original URL.
bool open_embed_context(const DownloadSource& source, Utf8View link, UiRect anchor) noexcept {
    auto* q = ensure_owner();
    if (!link.bytes)
        link = {source.url, source.url_bytes};
    if (!q || link.bytes > sizeof(q->menu_link) || !source_current(source) || !dismiss_popup())
        return false;
    q->menu_valid = false;
    const auto kind = embed_kind(source);
    const ReceivedAttachmentMenuFacts facts{kind, link, false, true};
    const u32 count = received_attachment_menu(facts, {q->menu_items, kReceivedAttachmentMenuItems});
    if (!count || !open_message_media_context_popup(source.message, anchor, {q->menu_items, count}))
        return false;
    q->menu = source;
    q->menu_kind = kind;
    copy_bytes(q->menu_link, link.data, link.bytes);
    q->menu_link_bytes = static_cast<u16>(link.bytes);
    q->menu_opens = g_app.message_context.opens;
    q->menu_valid = true;
    return application_request_frame();
}

} // namespace
namespace downloads_detail {

// A right-clicked embed: Save when its media can be fetched, else only links.
bool embed_context(float x, float y, UiRect anchor) noexcept {
    DownloadSource shown{}, media{};
    u32 index = 0;
    bool snapshot = false;
    if (!embed_image_at(x, y, &shown, &index, &snapshot))
        return false;
    return capture_embed(shown.message, index, snapshot, &media)
               ? open_embed_context(media, {shown.url, shown.url_bytes}, anchor)
               : open_sticker_context(shown, anchor);
}
// Copy Image reads the largest decoded tier the renderer still holds; the
// viewer's 2048 DIP decode bound caps it.
bool copy_image(const DownloadSource& source, MediaAttachmentFormat format) noexcept {
    constexpr u32 capacity = 2048u * 2048u * 4u;
    auto* pixels =
        static_cast<u8*>(commit_pages(capacity));
    if (!pixels)
        return false;
    static constexpr MediaResolutionTier tiers[]{MediaResolutionTier::original, MediaResolutionTier::px2048,
                                          MediaResolutionTier::px1024,   MediaResolutionTier::px512,
                                          MediaResolutionTier::px256,    MediaResolutionTier::px128,
                                          MediaResolutionTier::px64};
    u32 width = 0, height = 0;
    bool read = false;
    for (u32 index = 0; !read && index < sizeof(tiers) / sizeof(tiers[0]); ++index)
        read = renderer_read_media_rgba(&g_app.renderer,
                                        media_asset_attachment(source.attachment, format, tiers[index]),
                                        {pixels, capacity}, &width, &height);
    const bool copied = read && clipboard_write_image(g_app.window, pixels, width, height, width * 4,
                                                      {source.url, source.url_bytes});
    pages_release(pixels);
    application_toast_result(copied, kToastImageCopied, kToastImageCopyFailed);
    if (!copied)
        log_write(LogLevel::warning, "downloads",
                  read ? "copy_image_clipboard_failed" : "copy_image_not_resident");
    return copied;
}
void copy_link(const Utf8View& link) noexcept {
    if (!application_copy_text(link, kToastLinkCopied, kToastLinkCopyFailed))
        log_write(LogLevel::warning, "downloads", "copy_link_failed");
}

} // namespace downloads_detail
} // namespace maicord::app_detail

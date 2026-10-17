#include "application_downloads.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_composer.hpp"
#include "application_content_input.hpp"
#include "application_media_policy.hpp"
#include "application_message_interaction.hpp"
#include "application_account_menu.hpp"
#include "application_image_viewer.hpp"
#include "application_message_forward.hpp"
#include "application_quick_switcher.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_media_picker.hpp"
#include "application_profile.hpp"
#include "application_video.hpp"
#include "application_surface_motion.hpp"
#include "application_settings.hpp"
#include "application_plugins.hpp"
#include "application_theme_editor.hpp"
#include "maicord/app/account_download.hpp"
#include "maicord/app/received_attachment_open.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/protocol/attachment_download.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/cdn_url.hpp"
#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include <shlobj.h>

namespace maicord::app_detail {
namespace {
constexpr u32 kDownloadJobs = 8, kNoDownload = ~u32{0};
enum class JobPhase : u8 {
    warning,
    queued,
    choosing,
    resolving,
    transferring,
    saved,
    failed,
    cancelled
};
struct DownloadSource {
    u64 scope, profile, self, channel, message, attachment, origin_channel, origin_message, fingerprint,
        bytes;
    MessageHandle handle;
    ApplicationSearchSourceTicket search_source;
    u32 revision;
    u16 url_bytes, filename_bytes;
    bool snapshot, risky, search_result;
    // Embed media: attachment is zero, url is its media-proxy URL and bytes is
    // kHttpsFileDownloadSizeFromResponse.
    bool embed;
    u8 embed_index;
    char url[2048], filename[512];
};
struct DownloadJob {
    DownloadSource source;
    FileSnapshot destination;
    wchar_t* path;
    u64 id;
    HttpsFileDownloadResult transfer;
    ReceivedAttachmentOpen resolution;
    u32 path_units;
    char chosen_name[512];
    u16 chosen_name_bytes;
    JobPhase phase;
    bool occupied, admitted, cancel_requested, destination_exists, overwrite_authorized, approved,
        after_choice_warning, force_refresh;
};
} // namespace
struct ApplicationDownloads {
    DownloadJob jobs[kDownloadJobs];
    DownloadSource context;
    NativeFilePicker picker;
    UiDownloadScene scene;
    UiDownloadSource source;
    UiDownloadContent content;
    UiDownloadTarget pressed;
    UiWidgetHandle pressed_widget;
    UiRect pressed_bounds, pressed_viewport, pressed_panel;
    u32 pressed_layout_revision;
    // The right-clicked attachment behind the message menu's media rows.
    DownloadSource menu;
    PopupItem menu_items[kReceivedAttachmentMenuItems];
    u32 menu_opens;
    ReceivedAttachmentKind menu_kind;
    MediaAttachmentFormat menu_format;
    bool menu_sticker; // menu.url is a sticker image; menu.attachment is zero
    bool menu_embed;   // menu is embed media; its link rows use menu_link
    u16 menu_link_bytes;
    char menu_link[2048];
    AccountUiRuntime* account_ui;
    void* account_instance;
    u64 scope, profile, self, warning_id, wake_at;
    u32 active_job, picker_job, revision;
    bool active, captured, closing, menu_valid, inline_capture, close_window;
    // A save failed or needs confirmation: the next pump opens the panel.
    bool reveal;
};
namespace {
bool account_current(const ApplicationDownloads& q) noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           q.account_ui == g_app.account_ui && q.account_instance == g_app.account.instance &&
           q.scope == g_app.plugin_account_scope_serial && q.profile == g_app.account.status.profile_id &&
           q.self == g_app.account_ui->chat_state.self_user_id;
}
bool account_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && g_app.plugin_account_scope_serial && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id && g_app.account.api.download;
}
bool foreground_form() noexcept {
    return application_settings_active() || application_theme_editor_active() ||
           (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
            g_app.renderer.forum_form->scene->active) ||
           (g_app.renderer.text_form && g_app.renderer.text_form->scene &&
            g_app.renderer.text_form->scene->active);
}
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
bool url_current(Utf8View url, u64 origin, u64 attachment) noexcept {
    u8 target[kCdnMaximumTargetBytes], prefix[80];
    const auto parsed = parse_signed_attachment_url(url, attachment, {target, sizeof(target)});
    if (!parsed.ok())
        return false;
    ProtocolWriter writer{{prefix, sizeof(prefix)}, 0};
    return protocol_writer_append_c_string(&writer, parsed.ephemeral ? "/ephemeral-attachments/"
                                                                     : "/attachments/") &&
           protocol_writer_append_u64(&writer, origin) && protocol_writer_append_literal(&writer, "/") &&
           protocol_writer_append_u64(&writer, attachment) &&
           protocol_writer_append_literal(&writer, "/") && parsed.target_bytes > writer.used &&
           bytes_equal(target, prefix, writer.used);
}
bool capture_payload(const MessageRecord& record, MessageHandle handle, const MessageRichStore& store,
                     u64 attachment_id, bool snapshot, DownloadSource* out) noexcept {
    if (!account_ready() || !attachment_id ||
        record.delivery != MessageDeliveryState::authoritative || !channel_current(record.channel_id))
        return false;
    const auto* message = &record;
    const u64 message_id = message->id;
    const auto* outer = message_rich_store_find(store, message_id);
    const auto* rich = snapshot ? message_rich_store_snapshot(store, message_id) : outer;
    if (!outer || !rich || rich->message_revision != message->revision ||
        outer->message_revision != message->revision ||
        (snapshot &&
         (outer->reference.type != 1 || !outer->reference.channel_id || !outer->reference.message_id)))
        return false;
    const auto origin = snapshot ? outer->reference.channel_id : message->channel_id;
    for (u32 i = 0; i < rich->attachment_count; ++i) {
        const auto& a = rich->attachments[i];
        if (a.id != attachment_id)
            continue;
        const auto url = message_rich_store_string(store, a.url);
        if (!url.data || !url.bytes || url.bytes >= sizeof(out->url) ||
            !url_current(url, origin, attachment_id))
            return false;
        DownloadSource source{};
        const auto name = attachment_download_safe_filename(message_rich_store_string(store, a.filename),
                                                            {source.filename, sizeof(source.filename)});
        if (!name.ok() || !name.bytes)
            return false;
        source.scope = g_app.plugin_account_scope_serial;
        source.profile = g_app.account.status.profile_id;
        source.self = g_app.account_ui->chat_state.self_user_id;
        source.channel = message->channel_id;
        source.message = message_id;
        source.attachment = attachment_id;
        source.origin_channel = origin;
        source.origin_message = snapshot ? outer->reference.message_id : message_id;
        source.handle = handle;
        source.revision = message->revision;
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
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    return message && message->channel_id == g_app.account_ui->active_channel_id &&
           capture_payload(*message, found.handle, g_app.account_ui->message_rich_store, attachment_id,
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
bool media_downloadable(Utf8View url) noexcept {
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
    const auto* outer = message_rich_store_find(store, record.id);
    const auto* rich = snapshot ? message_rich_store_snapshot(store, record.id) : outer;
    MessageRichEmbedMediaSelection selected{};
    if (!outer || !rich || rich->message_revision != record.revision ||
        outer->message_revision != record.revision || embed_index >= rich->embed_count ||
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
    source.scope = g_app.plugin_account_scope_serial;
    source.profile = g_app.account.status.profile_id;
    source.self = g_app.account_ui->chat_state.self_user_id;
    source.channel = source.origin_channel = record.channel_id;
    source.message = source.origin_message = record.id;
    source.handle = handle;
    source.revision = record.revision;
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
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    return message && message->channel_id == g_app.account_ui->active_channel_id &&
           capture_embed_payload(*message, found.handle, g_app.account_ui->message_rich_store, embed_index,
                                 snapshot, out);
}
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
bool source_media_allowed(const DownloadSource& source, bool activation = false) noexcept {
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
    if (source.embed) {
        if (source.embed_index >= rich->embed_count)
            return false;
        const auto owner = message_media_owner(*message, outer);
        const auto key = message_media_reveal_key(source.message, MessageMediaSource::embed,
                                                  source.embed_index, source.snapshot);
        const bool explicit_media = message_rich_embed_explicit(rich->embeds[source.embed_index]);
        return activation ? !application_media_activation_consumed(*scene, owner, source.message, key,
                                                                   false, explicit_media)
                          : message_scene_media_obscure(*scene, owner, source.message, key, false,
                                                        explicit_media) == MessageMediaObscure::none;
    }
    for (u32 index = 0; index < rich->attachment_count; ++index) {
        const auto& attachment = rich->attachments[index];
        if (attachment.id != source.attachment)
            continue;
        const auto owner = message_media_owner(*message, outer);
        const auto key = message_media_reveal_key(source.message, MessageMediaSource::attachment,
                                                  index, source.snapshot);
        const bool spoiler = message_rich_attachment_spoiler(*store, attachment),
                   explicit_media = message_rich_attachment_explicit(attachment);
        // Only a user activation may reveal or request age confirmation. Pending
        // admission rechecks policy without creating either side effect.
        return activation
                   ? !application_media_activation_consumed(*scene, owner, source.message, key,
                                                             spoiler, explicit_media)
                   : message_scene_media_obscure(*scene, owner, source.message, key, spoiler,
                                                  explicit_media) == MessageMediaObscure::none;
    }
    return false;
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
        VirtualFree(j.path, 0, MEM_RELEASE);
    }
    j.path = nullptr;
    j.path_units = 0;
    j.chosen_name_bytes = 0;
}
void retire_job(DownloadJob& j) noexcept {
    release_path(j);
    SecureZeroMemory(&j, sizeof(j));
}
ApplicationDownloads* ensure_owner() noexcept {
    if (g_app.downloads)
        return account_current(*g_app.downloads) && !g_app.downloads->closing ? g_app.downloads : nullptr;
    if (!account_ready())
        return nullptr;
    auto* q = static_cast<ApplicationDownloads*>(
        VirtualAlloc(nullptr, sizeof(ApplicationDownloads), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
DownloadJob* find_job(ApplicationDownloads& q, u64 id, u32* index = nullptr) noexcept {
    for (u32 i = 0; i < kDownloadJobs; ++i)
        if (q.jobs[i].occupied && q.jobs[i].id == id) {
            if (index)
                *index = i;
            return &q.jobs[i];
        }
    return nullptr;
}
bool terminal(JobPhase p) noexcept {
    return p == JobPhase::saved || p == JobPhase::failed || p == JobPhase::cancelled;
}
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
bool scene_current(void* opaque) noexcept {
    const auto* q = static_cast<const ApplicationDownloads*>(opaque);
    return q && q == g_app.downloads && q->active && account_current(*q) && !q->closing &&
           !foreground_form();
}
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
                    j.chosen_name_bytes ? Utf8View{j.chosen_name, j.chosen_name_bytes}
                                        : Utf8View{j.source.filename, j.source.filename_bytes},
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
void revoke_capture(ApplicationDownloads& q) noexcept {
    const bool owned = q.captured || q.scene.widgets.captured.generation;
    g_app.downloads_pointer_tail = g_app.downloads_pointer_tail || q.captured;
    q.captured = false;
    q.pressed = {};
    q.pressed_widget = {};
    q.pressed_bounds = {};
    ui_widget_manager_capture(&q.scene.widgets, ui_widget_invalid_handle());
    if (owned && GetCapture() == g_app.window)
        ReleaseCapture();
}
// Why a save failed, without its path or URL.
void log_failure(const HttpsFileDownloadResult& transfer) noexcept {
    log_u64(LogLevel::warning, "download", "failed_error", static_cast<u64>(transfer.error));
    if (transfer.native_error)
        log_u64(LogLevel::warning, "download", "failed_native_error", transfer.native_error);
    if (transfer.status_code)
        log_u64(LogLevel::warning, "download", "failed_status", transfer.status_code);
}
void fail_job(ApplicationDownloads& q, DownloadJob& j, HttpsFileDownloadError error) noexcept {
    j.phase = JobPhase::failed;
    j.transfer.error = error;
    j.resolution = {};
    log_failure(j.transfer);
    q.reveal = true;
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
        j.resolution = {};
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
    if (slot == kNoDownload || g_app.downloads_lifetime_serial == ~u64{0})
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
} // namespace
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
    if (GetCapture() == g_app.window)
        ReleaseCapture();
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
    q->content = {};
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
            q->content.filename = j->chosen_name_bytes
                                      ? Utf8View{j->chosen_name, j->chosen_name_bytes}
                                      : Utf8View{j->source.filename, j->source.filename_bytes};
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
                j.path = static_cast<wchar_t*>(VirtualAlloc(nullptr, (units + 1) * sizeof(wchar_t),
                                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        revoke_capture(q);
        ui_download_scene_page(&q.scene, target.action == UiDownloadAction::previous
                                             ? (q.scene.page ? q.scene.page - 1 : 0)
                                             : q.scene.page + 1);
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
        if (source_current(j->source) && !source_media_allowed(j->source, true))
            return;
        if (!source_current(j->source))
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
        j->transfer = {};
        j->resolution = {};
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
bool attachment_at(float x, float y, bool save_only, DownloadSource* out, UiWidgetHandle focused = {},
                   UiRect* bounds = nullptr, UiWidgetHandle* handle = nullptr) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active)
        return false;
    const auto& scene = g_app.message_scene;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 r = 0; r < scene.count; ++r)
        for (u32 s = 0; s < 2; ++s) {
            const auto* row = s ? message_scene_snapshot_row(scene, scene.rows[r]) : &scene.rows[r];
            if (!row)
                continue;
            const auto* message = message_store_resolve(g_app.message_view.store, row->message);
            const auto* rich = message ? (s ? message_rich_store_snapshot(store, message->id)
                                            : message_rich_store_find(store, message->id))
                                       : nullptr;
            if (!rich || rich->message_revision != message->revision)
                continue;
            for (u32 a = 0; a < rich->attachment_count; ++a) {
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, a);
                if (focused.generation) {
                    if (widget.slot == focused.slot && widget.generation == focused.generation &&
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
                    const auto rectangle = ui_rect_intersection(
                        card.bounds,
                        ui_rect_intersection(node->clip_rectangle,
                                             ui_rect_intersection(row_node->rectangle, scene.viewport)));
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
                rectangle = ui_rect_intersection(
                    rectangle,
                    ui_rect_intersection(node->clip_rectangle,
                                         ui_rect_intersection(row_node->rectangle, scene.viewport)));
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
// The attachment record behind a captured source, revalidated by identity.
const MessageRichAttachment* source_attachment(const DownloadSource& source) noexcept {
    if (!g_app.account_ui || !source_current(source))
        return nullptr;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = source.snapshot ? message_rich_store_snapshot(store, source.message)
                                       : message_rich_store_find(store, source.message);
    for (u32 index = 0; rich && index < rich->attachment_count; ++index)
        if (rich->attachments[index].id == source.attachment)
            return &rich->attachments[index];
    return nullptr;
}
ReceivedAttachmentKind source_kind(const DownloadSource& source, MediaAttachmentFormat* format) noexcept {
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* attachment = source_attachment(source);
    *format =
        attachment ? message_rich_attachment_format(store, *attachment) : MediaAttachmentFormat::unknown;
    const auto* rich = source.snapshot ? message_rich_store_snapshot(store, source.message)
                                       : message_rich_store_find(store, source.message);
    switch (*format) {
    case MediaAttachmentFormat::png:
    case MediaAttachmentFormat::jpeg:
    case MediaAttachmentFormat::webp:
        return ReceivedAttachmentKind::image;
    case MediaAttachmentFormat::gif:
        return ReceivedAttachmentKind::animated_image;
    case MediaAttachmentFormat::mp4:
        return ReceivedAttachmentKind::video;
    default:
        if (media_attachment_format_audio_supported(*format))
            return rich && message_rich_record_voice(*rich) ? ReceivedAttachmentKind::voice_message
                                                            : ReceivedAttachmentKind::audio;
        return ReceivedAttachmentKind::file;
    }
}
// The official client's attachment groups merged into the message menu, before Copy ID.
bool open_context(DownloadSource source, UiRect anchor) noexcept {
    auto* q = ensure_owner();
    if (!q || !source_current(source) || !dismiss_popup())
        return false;
    q->menu_valid = false;
    MediaAttachmentFormat format{};
    const auto kind = source_kind(source, &format);
    const ReceivedAttachmentMenuFacts facts{
        kind, {source.url, source.url_bytes}, kind == ReceivedAttachmentKind::image, true};
    const u32 count = received_attachment_menu(facts, {q->menu_items, kReceivedAttachmentMenuItems});
    // Plugins see the attachment's index in the message, its URL and filename.
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = source.snapshot ? message_rich_store_snapshot(store, source.message)
                                       : message_rich_store_find(store, source.message);
    u32 index = 0;
    while (rich && index < rich->attachment_count && rich->attachments[index].id != source.attachment)
        ++index;
    const ApplicationPluginMenuExtra plugin{MC_MENU_ATTACHMENT, index, 0, false, {source.url, source.url_bytes},
                                            {source.filename, source.filename_bytes}};
    const bool attachment = source.attachment && rich && index < rich->attachment_count;
    if (!count || !open_message_media_context_popup(source.message, anchor, {q->menu_items, count},
                                                    attachment ? &plugin : nullptr))
        return false;
    q->menu = source;
    q->menu_kind = kind;
    q->menu_format = format;
    q->menu_sticker = false;
    q->menu_embed = false;
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
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 r = 0; r < scene.count; ++r)
        for (u32 s = 0; s < 2; ++s) {
            const auto* row = s ? message_scene_snapshot_row(scene, scene.rows[r]) : &scene.rows[r];
            const auto* message =
                row ? message_store_resolve(g_app.message_view.store, row->message) : nullptr;
            const auto* rich = message ? (s ? message_rich_store_snapshot(store, message->id)
                                            : message_rich_store_find(store, message->id))
                                       : nullptr;
            if (!rich || rich->message_revision != message->revision ||
                message->channel_id != g_app.account_ui->active_channel_id)
                continue;
            const auto* row_node = message_scene_layout_node(scene, row->widget);
            for (u32 i = 0; row_node && i < rich->sticker_count; ++i) {
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::sticker, i);
                const auto* node =
                    message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::sticker, i);
                if (!node || !ui_widget_manager_input_enabled(scene.widgets, widget))
                    continue;
                const UiRect visible = ui_rect_intersection(
                    node->rectangle,
                    ui_rect_intersection(node->clip_rectangle,
                                         ui_rect_intersection(row_node->rectangle, scene.viewport)));
                const bool hit = focused.generation ? widget.slot == focused.slot &&
                                                          widget.generation == focused.generation
                                                    : ui_rect_contains(visible, x, y);
                if (!hit)
                    continue;
                const auto& sticker = rich->stickers[i];
                u8 target[64]{};
                const auto written =
                    sticker.format_type == 3
                        ? CdnTargetResult{0, {}, CdnTargetError::unsupported_format}
                        : write_sticker_cdn_target(sticker.id, sticker.format_type,
                                                   {target, sizeof(target)});
                if (!written.ok())
                    return false;
                // Message sticker images are served from the media proxy.
                DownloadSource source{};
                ProtocolWriter writer{{reinterpret_cast<u8*>(source.url), sizeof(source.url)}, 0};
                if (!protocol_writer_append_literal(&writer, "https://media.discordapp.net") ||
                    !protocol_writer_append_bytes(&writer, target, written.bytes))
                    return false;
                source.url_bytes = static_cast<u16>(writer.used);
                source.scope = g_app.plugin_account_scope_serial;
                source.profile = g_app.account.status.profile_id;
                source.self = g_app.account_ui->chat_state.self_user_id;
                source.channel = message->channel_id;
                source.message = message->id;
                source.revision = message->revision;
                *out = source;
                return true;
            }
        }
    return false;
}
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
            const auto* row = s ? message_scene_snapshot_row(scene, scene.rows[r]) : &scene.rows[r];
            const auto* message =
                row ? message_store_resolve(g_app.message_view.store, row->message) : nullptr;
            const auto* rich = message ? (s ? message_rich_store_snapshot(store, message->id)
                                            : message_rich_store_find(store, message->id))
                                       : nullptr;
            const auto* row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
            if (!rich || !row_node || rich->message_revision != message->revision ||
                message->channel_id != g_app.account_ui->active_channel_id)
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
                const UiRect visible = ui_rect_intersection(
                    preview, ui_rect_intersection(node->clip_rectangle,
                                                  ui_rect_intersection(row_node->rectangle, scene.viewport)));
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
                source.scope = g_app.plugin_account_scope_serial;
                source.profile = g_app.account.status.profile_id;
                source.self = g_app.account_ui->chat_state.self_user_id;
                source.channel = message->channel_id;
                source.message = message->id;
                source.revision = message->revision;
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
bool open_sticker_context(const DownloadSource& source, UiRect anchor) noexcept {
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
    q->menu_sticker = true;
    q->menu_embed = false;
    q->menu_opens = g_app.message_context.opens;
    q->menu_valid = true;
    return application_request_frame();
}
// What embed media is, for its menu labels: an animation, a video or an image.
ReceivedAttachmentKind embed_kind(const DownloadSource& source) noexcept {
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = source.snapshot ? message_rich_store_snapshot(store, source.message)
                                       : message_rich_store_find(store, source.message);
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
    q->menu_sticker = false;
    q->menu_embed = true;
    copy_bytes(q->menu_link, link.data, link.bytes);
    q->menu_link_bytes = static_cast<u16>(link.bytes);
    q->menu_opens = g_app.message_context.opens;
    q->menu_valid = true;
    return application_request_frame();
}
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
        static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pixels)
        return false;
    constexpr MediaResolutionTier tiers[]{MediaResolutionTier::original, MediaResolutionTier::px2048,
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
    VirtualFree(pixels, 0, MEM_RELEASE);
    if (!copied)
        log_write(LogLevel::warning, "downloads",
                  read ? "copy_image_clipboard_failed" : "copy_image_not_resident");
    return copied;
}
} // namespace
bool application_downloads_pump() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return true;
    if (!account_current(*q) && !q->closing)
        application_downloads_prepare_account_stop();
    if (!q->closing && q->picker_job != kNoDownload) {
        auto& choosing = q->jobs[q->picker_job];
        if (choosing.phase == JobPhase::choosing && !choosing.cancel_requested &&
            (!source_current(choosing.source) || !source_media_allowed(choosing.source))) {
            choosing.cancel_requested = true;
            fail_job(*q, choosing, HttpsFileDownloadError::invalid_input);
            native_file_picker_cancel(&q->picker);
        }
    }
    picker_complete(*q);
    if (q->active_job != kNoDownload) {
        auto& j = q->jobs[q->active_job];
        if (q->account_instance == g_app.account.instance && g_app.account.api.download) {
            const auto state = g_app.account.api.download(q->account_instance,
                                                          AccountDownloadOperation::status, j.id, nullptr);
            if (state.id == j.id) {
                if (!bytes_equal(&j.transfer, &state.transfer, sizeof(j.transfer))) {
                    j.transfer = state.transfer;
                    changed(*q);
                }
                if (state.transfer.phase == HttpsFileDownloadPhase::complete) {
                    const auto released = g_app.account.api.download(
                        q->account_instance, AccountDownloadOperation::release, j.id, nullptr);
                    if (released.released) {
                        j.admitted = false;
                        q->active_job = kNoDownload;
                        j.phase = state.transfer.ok() ? JobPhase::saved
                                  : state.transfer.error == HttpsFileDownloadError::cancelled
                                      ? JobPhase::cancelled
                                      : JobPhase::failed;
                        if (j.phase == JobPhase::failed) {
                            log_failure(state.transfer);
                            q->reveal = true;
                        }
                        changed(*q);
                    }
                }
            }
        } else {
            j.admitted = false;
            j.phase = JobPhase::cancelled;
            q->active_job = kNoDownload;
            changed(*q);
        }
    }
    if (q->closing) {
        const bool close = q->close_window;
        application_downloads_close();
        if (application_downloads_release() && close && g_app.window)
            PostMessageW(g_app.window, WM_CLOSE, 0, 0);
        return true;
    }
    // The save dialog and a transfer run side by side, one of each at a time, so
    // a save chosen during another download asks for its location at once.
    bool resolution_pending = false;
    for (u32 i = 0; i < kDownloadJobs; ++i) {
        auto& j = q->jobs[i];
        if (!j.occupied || terminal(j.phase) || i == q->picker_job || i == q->active_job)
            continue;
        if (!source_current(j.source) || !source_media_allowed(j.source)) {
            fail_job(*q, j, HttpsFileDownloadError::invalid_input);
            continue;
        }
        if (j.phase == JobPhase::warning) {
            if (!q->warning_id) {
                q->warning_id = j.id;
                changed(*q);
            }
            continue;
        }
        if (j.phase == JobPhase::queued) {
            if (q->picker_job != kNoDownload)
                continue;
            const auto error = native_file_picker_save_start(&q->picker, g_app.window,
                                                             {j.source.filename, j.source.filename_bytes},
                                                             utf8_literal("Save attachment"));
            if (error != NativeFilePickerError::none) {
                fail_job(*q, j, HttpsFileDownloadError::file_open);
                continue;
            }
            q->picker_job = i;
            j.phase = JobPhase::choosing;
            changed(*q);
            continue;
        }
        if (j.phase != JobPhase::resolving || !j.approved || !j.path || q->active_job != kNoDownload ||
            resolution_pending)
            continue;
        const ReceivedAttachmentOpenIdentity identity{
            j.source.scope,      j.source.channel,  j.source.message,
            j.source.attachment, j.source.revision, j.source.snapshot ? j.source.origin_channel : 0};
        Utf8View ready{};
        const Utf8View original{j.source.url, j.source.url_bytes};
        // Embed media is fetched by the media-proxy URL it was captured with;
        // only signed attachment links are refreshed first.
        auto result = ReceivedAttachmentOpenResult::ready;
        if (j.source.embed)
            ready = original;
        else
            result = j.resolution.pending
                         ? received_attachment_open_resume(
                               &j.resolution, identity, original, &g_app.media_urls,
                               clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready)
                         : received_attachment_open_begin(&j.resolution, identity, original,
                                                          &g_app.media_urls, clock_monotonic_milliseconds(),
                                                          clock_unix_milliseconds() / 1000, &ready,
                                                          j.force_refresh);
        if (result == ReceivedAttachmentOpenResult::pending) {
            resolution_pending = true;
            continue;
        }
        if (result != ReceivedAttachmentOpenResult::ready || !source_current(j.source) ||
            !source_media_allowed(j.source) ||
            (!j.source.embed && !url_current(ready, j.source.origin_channel, j.source.attachment))) {
            fail_job(*q, j,
                     result == ReceivedAttachmentOpenResult::stale ? HttpsFileDownloadError::invalid_input
                                                                   : HttpsFileDownloadError::http);
            continue;
        }
        AccountDownloadRequest request{};
        request.url = ready;
        request.path = {j.path, j.path_units};
        request.source_channel_id = j.source.embed ? 0 : j.source.origin_channel;
        request.attachment_id = j.source.attachment;
        request.expected_bytes = j.source.bytes;
        request.destination = j.destination;
        request.destination_exists = j.destination_exists;
        request.overwrite_authorized = j.overwrite_authorized;
        const auto started = g_app.account.api.download(g_app.account.instance,
                                                        AccountDownloadOperation::start, j.id, &request);
        if (started.submit_error != HttpsFileDownloadError::none || started.id != j.id) {
            if (!started.released && started.id == j.id) {
                j.admitted = true;
                j.phase = JobPhase::transferring;
                j.transfer = started.transfer;
                j.transfer.error = started.submit_error;
                q->active_job = i;
                changed(*q);
                continue;
            }
            fail_job(*q, j,
                     started.submit_error == HttpsFileDownloadError::none ? HttpsFileDownloadError::worker
                                                                          : started.submit_error);
            continue;
        }
        j.force_refresh = false;
        j.admitted = true;
        j.phase = JobPhase::transferring;
        j.transfer = started.transfer;
        q->active_job = i;
        changed(*q);
    }
    if (q->reveal) {
        q->reveal = false;
        if (!q->active)
            application_downloads_open();
    }
    q->wake_at = q->picker_job != kNoDownload || (q->active && q->active_job != kNoDownload)
                     ? deadline_after(clock_monotonic_milliseconds(), 100)
                     : 0;
    return application_downloads_synchronize();
}
bool application_downloads_save_target(float x, float y) noexcept {
    DownloadSource source{};
    return attachment_at(x, y, true, &source);
}
bool application_downloads_save_press(float x, float y) noexcept {
    DownloadSource source{};
    UiRect bounds{};
    UiWidgetHandle widget{};
    if (!attachment_at(x, y, true, &source, {}, &bounds, &widget))
        return false;
    auto* q = ensure_owner();
    if (!q)
        return true;
    q->context = source;
    q->inline_capture = true;
    q->pressed_bounds = bounds;
    q->pressed_widget = widget;
    ui_widget_manager_capture(&g_app.message_scene.widgets, widget);
    ui_widget_manager_focus(&g_app.message_scene.widgets, widget);
    application_request_frame();
    if (g_app.window)
        SetCapture(g_app.window);
    return true;
}
bool application_downloads_save_focused() noexcept {
    DownloadSource source{};
    return g_app.message_scene.widgets.focused.generation &&
           attachment_at(0, 0, false, &source, g_app.message_scene.widgets.focused) && begin_source(source);
}
bool application_downloads_context_at(float x, float y) noexcept {
    DownloadSource source{};
    const UiRect anchor{x, y, x + 1, y + 1};
    return attachment_at(x, y, false, &source) ? open_context(source, anchor)
           : sticker_at(x, y, {}, &source)     ? open_sticker_context(source, anchor)
                                               : embed_context(x, y, anchor);
}
bool application_downloads_context_focused() noexcept {
    DownloadSource source{};
    const auto focused = g_app.message_scene.widgets.focused;
    return attachment_at(0, 0, false, &source, focused) ? open_context(source, g_app.message_scene.viewport)
           : sticker_at(0, 0, focused, &source) ? open_sticker_context(source, g_app.message_scene.viewport)
                                                : false;
}
bool application_downloads_context_action(u32 action) noexcept {
    auto* q = g_app.downloads;
    // Only the message menu this owner opened, still showing, carries the rows.
    if (!received_attachment_menu_action(action) || !q || !q->menu_valid ||
        g_app.popup_owner_kind != PopupOwnerKind::message || !g_app.popup_scene.active ||
        !g_app.message_context.active || g_app.message_context.opens != q->menu_opens ||
        g_app.message_context.message_id != q->menu.message)
        return false;
    const auto source = q->menu;
    q->menu_valid = false;
    if (!dismiss_popup())
        return false;
    const Utf8View url{source.url, source.url_bytes};
    if (q->menu_sticker) {
        const auto found = message_store_find(g_app.message_view.store, source.message);
        const auto* message =
            found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (!message || message->revision != source.revision)
            return true;
        if (action == kReceivedAttachmentActionCopyLink && !clipboard_write_utf8(g_app.window, url))
            log_write(LogLevel::warning, "downloads", "copy_link_failed");
        if (action == kReceivedAttachmentActionOpenLink)
            application_open_content_url(url);
        return true;
    }
    if (!source_current(source))
        return true;
    if (q->menu_embed) {
        const Utf8View link{q->menu_link, q->menu_link_bytes};
        if (action == kReceivedAttachmentActionSave)
            return begin_source(source);
        if (action == kReceivedAttachmentActionCopyLink && !clipboard_write_utf8(g_app.window, link))
            log_write(LogLevel::warning, "downloads", "copy_link_failed");
        if (action == kReceivedAttachmentActionOpenLink)
            application_open_content_url(link);
        return true;
    }
    switch (action) {
    case kReceivedAttachmentActionSave:
        return begin_source(source);
    case kReceivedAttachmentActionCopyImage:
        if (q->menu_kind == ReceivedAttachmentKind::image)
            copy_image(source, q->menu_format);
        return true;
    case kReceivedAttachmentActionCopyLink:
        if (!clipboard_write_utf8(g_app.window, url))
            log_write(LogLevel::warning, "downloads", "copy_link_failed");
        return true;
    case kReceivedAttachmentActionOpenLink:
        application_received_attachment_open(source.message, source.attachment,
                                             source.snapshot ? source.origin_channel : 0);
        return true;
    default:
        return true;
    }
}
bool application_downloads_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (kind == UiInputKind::character &&
        ((g_app.downloads_held_key == 'J' && key == 10) || (g_app.downloads_held_key == 'S' && key == 19)))
        return true;
    if (ui_input_consume_key_tail(&g_app.downloads_held_key, {kind, key, repeated}))
        return true;
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::downloads, shortcut_chord_from_keyboard(key))) {
        g_app.downloads_held_key = key;
        if (!repeated)
            application_downloads_open();
        return true;
    }
    if (!application_downloads_active() && kind == UiInputKind::key_down &&
        g_app.context_focus == &g_app.message_scene.widgets &&
        shortcut_matches(ShortcutAction::save_attachment, shortcut_chord_from_keyboard(key)) &&
        application_downloads_save_focused()) {
        g_app.downloads_held_key = key;
        return true;
    }
    auto* q = g_app.downloads;
    if (!q || !q->active)
        return false;
    if (!scene_current(q)) {
        const bool yield = foreground_form();
        application_downloads_close();
        return !yield;
    }
    if (kind == UiInputKind::key_down) {
        if (key == VK_ESCAPE) {
            g_app.downloads_held_key = key;
            application_downloads_close();
        } else if (key == VK_TAB) {
            g_app.downloads_held_key = key;
            ui_widget_manager_focus(&q->scene.widgets, ui_widget_manager_next_focus(
                                                           q->scene.widgets, q->scene.widgets.focused,
                                                           (GetKeyState(VK_SHIFT) & 0x8000) != 0, true));
            application_request_frame();
        } else if ((key == VK_RETURN || key == VK_SPACE) && !repeated) {
            g_app.downloads_held_key = key;
            activate(*q, ui_download_scene_target(q->scene, q->scene.widgets.focused));
        } else if (key == VK_PRIOR || key == VK_NEXT)
            activate(*q, {key == VK_PRIOR ? UiDownloadAction::previous : UiDownloadAction::next});
    }
    return true;
}
bool application_downloads_filter(const MSG& message) noexcept {
    return message.hwnd == g_app.window && (message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
           ui_input_consume_key_tail(
               &g_app.downloads_held_key,
               {message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up,
                static_cast<u32>(message.wParam), (message.lParam & (1ll << 30)) != 0});
}
bool application_downloads_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* q = g_app.downloads;
    if (message == WM_LBUTTONDOWN && (!q || !q->active) && g_app.renderer_ready && !g_app.login.visible &&
        !g_app.image_viewer.scene.active && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
        !application_message_forward_active() && !application_quick_switcher_active() &&
        !application_media_picker_active() && !application_search_active() &&
        !application_pinned_messages_active() &&
        !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active)) {
        const float scale = 96.f / maximum(1u, g_app.renderer.dpi);
        if (application_downloads_save_press(static_cast<short>(LOWORD(lparam)) * scale,
                                             static_cast<short>(HIWORD(lparam)) * scale))
            return true;
    }
    if (q && q->inline_capture) {
        if (message == WM_LBUTTONUP) {
            const auto pressed = q->context;
            const auto widget = q->pressed_widget;
            const auto bounds = q->pressed_bounds;
            const auto held = g_app.message_scene.widgets.captured;
            const bool owned =
                held.generation && held.slot == widget.slot && held.generation == widget.generation;
            q->inline_capture = false;
            if (owned)
                ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
            if (GetCapture() == g_app.window)
                ReleaseCapture();
            const float scale = 96.f / maximum(1u, g_app.renderer.dpi);
            DownloadSource live{};
            UiRect current_bounds{};
            UiWidgetHandle current_widget{};
            if (owned && source_current(pressed) &&
                attachment_at(static_cast<short>(LOWORD(lparam)) * scale,
                              static_cast<short>(HIWORD(lparam)) * scale, true, &live, {}, &current_bounds,
                              &current_widget) &&
                current_widget.slot == widget.slot && current_widget.generation == widget.generation &&
                bytes_equal(&bounds, &current_bounds, sizeof(bounds)) && live.message == pressed.message &&
                live.attachment == pressed.attachment && live.snapshot == pressed.snapshot &&
                live.revision == pressed.revision && live.handle.generation == pressed.handle.generation &&
                live.fingerprint == pressed.fingerprint)
                begin_source(pressed);
            return true;
        }
        if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS ||
            message == WM_SIZE || message == WM_DPICHANGED) {
            const auto held = g_app.message_scene.widgets.captured;
            if (held.generation && held.slot == q->pressed_widget.slot &&
                held.generation == q->pressed_widget.generation)
                ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
            q->inline_capture = false;
            g_app.downloads_pointer_tail = true;
            if (GetCapture() == g_app.window)
                ReleaseCapture();
        } else if (message == WM_LBUTTONDOWN)
            q->inline_capture = false;
    }
    if (message == WM_LBUTTONDOWN && (!q || !q->active))
        g_app.downloads_pointer_tail = false;
    if (message == WM_LBUTTONUP && g_app.downloads_pointer_tail && (!q || !q->active)) {
        g_app.downloads_pointer_tail = false;
        return true;
    }
    if (!q || !q->active)
        return false;
    if (!scene_current(q)) {
        application_downloads_close();
        return true;
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS ||
        message == WM_SIZE || message == WM_DPICHANGED) {
        revoke_capture(*q);
        return false;
    }
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_LBUTTONDBLCLK ||
        message == WM_MOUSEHWHEEL)
        return true;
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONUP && message != WM_MOUSEMOVE &&
        message != WM_MOUSEWHEEL && message != WM_SETCURSOR)
        return false;
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (message == WM_MOUSEWHEEL)
        ScreenToClient(g_app.window, &point);
    if (message == WM_SETCURSOR) {
        if (LOWORD(lparam) != HTCLIENT || !GetCursorPos(&point) || !ScreenToClient(g_app.window, &point))
            return false;
    }
    const float scale = 96.f / maximum(1u, g_app.renderer.dpi);
    const auto target = ui_download_scene_hit_test(q->scene, point.x * scale, point.y * scale);
    if (message == WM_SETCURSOR) {
        application_set_cursor(ui_download_scene_cursor(q->scene, point.x * scale, point.y * scale));
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        revoke_capture(*q);
        ui_download_scene_page(&q->scene, static_cast<short>(HIWORD(wparam)) > 0
                                              ? (q->scene.page ? q->scene.page - 1 : 0)
                                              : q->scene.page + 1);
        application_request_frame();
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        bool paint = false;
        ui_download_scene_set_hover(&q->scene, point.x * scale, point.y * scale, &paint);
        if (paint)
            application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONDOWN) {
        q->pressed = target;
        q->captured = true;
        g_app.downloads_pointer_tail = false;
        q->pressed_widget = ui_widget_manager_hit_test(q->scene.widgets, point.x * scale, point.y * scale);
        q->pressed_viewport = q->scene.viewport;
        q->pressed_panel = q->scene.panel;
        q->pressed_layout_revision = q->scene.widgets.layout.revision;
        const auto* node = ui_widget_manager_layout_node(q->scene.widgets, q->pressed_widget);
        q->pressed_bounds = node ? node->rectangle : UiRect{};
        ui_widget_manager_capture(&q->scene.widgets, q->pressed_widget);
        ui_widget_manager_focus(&q->scene.widgets, q->pressed_widget);
        application_request_frame();
        if (g_app.window)
            SetCapture(g_app.window);
        return true;
    }
    const auto pressed = q->pressed;
    const auto widget = q->pressed_widget;
    const auto held = q->scene.widgets.captured;
    const auto* node = ui_widget_manager_layout_node(q->scene.widgets, widget);
    const bool backdrop =
        !widget.generation && pressed.action == UiDownloadAction::close &&
        target.action == UiDownloadAction::close &&
        q->pressed_layout_revision == q->scene.widgets.layout.revision &&
        bytes_equal(&q->scene.viewport, &q->pressed_viewport, sizeof(q->pressed_viewport)) &&
        bytes_equal(&q->scene.panel, &q->pressed_panel, sizeof(q->pressed_panel));
    const bool captured =
        q->captured &&
        (backdrop ||
         (held.generation && held.slot == widget.slot && held.generation == widget.generation && node &&
          bytes_equal(&node->rectangle, &q->pressed_bounds, sizeof(q->pressed_bounds))));
    revoke_capture(*q);
    g_app.downloads_pointer_tail = false;
    if (captured && pressed.id == target.id && pressed.action == target.action &&
        pressed.index == target.index)
        activate(*q, target);
    return true;
}
HANDLE application_downloads_notification() noexcept {
    return g_app.downloads ? g_app.downloads->picker.notification : nullptr;
}
u64 application_downloads_next_wake() noexcept {
    const auto* q = g_app.downloads;
    if (!q)
        return 0;
    return q->wake_at;
}
void application_downloads_prepare_account_stop() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return;
    if (q->inline_capture) {
        q->inline_capture = false;
        g_app.downloads_pointer_tail = true;
        const auto held = g_app.message_scene.widgets.captured;
        if (held.generation && held.slot == q->pressed_widget.slot &&
            held.generation == q->pressed_widget.generation)
            ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
        if (GetCapture() == g_app.window)
            ReleaseCapture();
    }
    for (u32 i = 0; i < kDownloadJobs; ++i)
        if (q->jobs[i].occupied && !terminal(q->jobs[i].phase))
            cancel_job(*q, i);
    q->closing = true;
    application_downloads_close();
    native_file_picker_cancel(&q->picker);
    q->wake_at = deadline_after(clock_monotonic_milliseconds(), 100);
}
bool application_downloads_defer_close() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return false;
    q->close_window = true;
    return !application_downloads_release();
}
bool application_downloads_release(u32 timeout) noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return true;
    application_downloads_prepare_account_stop();
    if (!native_file_picker_release(&q->picker, timeout))
        return false;
    q->picker_job = kNoDownload;
    if (q->active_job != kNoDownload && q->account_instance == g_app.account.instance &&
        g_app.account.api.download) {
        const auto result = g_app.account.api.download(
            q->account_instance, AccountDownloadOperation::release, q->jobs[q->active_job].id, nullptr);
        if (!result.released)
            return false;
    }
    ui_download_scene_release(&q->scene);
    for (auto& j : q->jobs)
        retire_job(j);
    SecureZeroMemory(q, sizeof(*q));
    VirtualFree(q, 0, MEM_RELEASE);
    g_app.downloads = nullptr;
    return true;
}
u32 application_downloads_owner_bytes() noexcept {
    const auto* q = g_app.downloads;
    if (!q)
        return 0;
    u32 bytes = sizeof(*q) + native_file_picker_committed_bytes(&q->picker);
    for (const auto& j : q->jobs)
        if (j.path)
            bytes += (j.path_units + 1) * sizeof(wchar_t);
    return bytes;
}
} // namespace maicord::app_detail

#pragma once
// Shared by downloads.cpp and its parts (downloads_*.cpp); private to them.
#include "downloads.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/state.hpp"
#include "../chat/messages/interaction.hpp"
#include "../chat/messages/forward.hpp"
#include "../shell/quick_switcher.hpp"
#include "../chat/search/search.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "picker.hpp"
#include "maicord/app/media/account_download.hpp"
#include "maicord/app/media/received_attachment_open.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/ui/media/download_scene.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/ui/chat/messages/received_attachment_menu.hpp"

namespace maicord::app_detail {
namespace downloads_detail {}
using namespace downloads_detail;
namespace downloads_detail {

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

} // namespace downloads_detail

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
    u16 menu_link_bytes; // embed media menus (menu.embed) name this link in their link rows
    char menu_link[2048];
    AccountUiRuntime* account_ui;
    void* account_instance;
    u64 scope, profile, self, warning_id, wake_at;
    u32 active_job, picker_job, revision;
    bool active, captured, closing, menu_valid, inline_capture, close_window;
    // A save failed or needs confirmation: the next pump opens the panel.
    bool reveal;
};

namespace downloads_detail {

bool account_current(const ApplicationDownloads& q) noexcept;
bool foreground_form() noexcept;
bool source_current(const DownloadSource& s) noexcept;
bool source_media_allowed(const DownloadSource& source, bool activation = false) noexcept;
void changed(ApplicationDownloads& q) noexcept;
void release_path(DownloadJob& j) noexcept;
ApplicationDownloads* ensure_owner() noexcept;
bool terminal(JobPhase p) noexcept;
bool scene_current(void* opaque) noexcept;
void revoke_capture(ApplicationDownloads& q) noexcept;
void page_step(ApplicationDownloads& q, bool previous) noexcept;
void log_failure(const HttpsFileDownloadResult& transfer) noexcept;
void toast_result(const DownloadJob& j) noexcept;
void fail_job(ApplicationDownloads& q, DownloadJob& j, HttpsFileDownloadError error) noexcept;
void cancel_job(ApplicationDownloads& q, u32 index) noexcept;
bool begin_source(const DownloadSource& source) noexcept;
void picker_complete(ApplicationDownloads& q) noexcept;
void activate(ApplicationDownloads& q, UiDownloadTarget target) noexcept;
bool attachment_at(float x, float y, bool save_only, DownloadSource* out, UiWidgetHandle focused = {},
                   UiRect* bounds = nullptr, UiWidgetHandle* handle = nullptr) noexcept;
bool open_context(const DownloadSource& source, const UiRect& anchor) noexcept;
bool sticker_at(float x, float y, UiWidgetHandle focused, DownloadSource* out) noexcept;
bool open_sticker_context(const DownloadSource& source, const UiRect& anchor) noexcept;
bool embed_context(float x, float y, UiRect anchor) noexcept;
bool copy_image(const DownloadSource& source, MediaAttachmentFormat format) noexcept;
void copy_link(const Utf8View& link) noexcept;

} // namespace downloads_detail
} // namespace maicord::app_detail

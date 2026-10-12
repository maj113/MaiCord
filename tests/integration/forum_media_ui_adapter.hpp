#pragma once
#include "forum_post_ui_adapter.hpp"
#include "application_forum_attachments.hpp"
namespace maicord {
bool forum_media_ui_adapter_enable() noexcept;
bool forum_media_ui_adapter_seed_drop() noexcept;
bool forum_media_ui_adapter_drop_consumed() noexcept;
bool forum_media_ui_adapter_paste_file() noexcept;
bool forum_media_ui_adapter_pump() noexcept;
bool forum_media_ui_adapter_upload(u64 committed, bool complete, bool failed = false) noexcept;
bool forum_media_ui_adapter_url(Utf8View*) noexcept;
bool forum_media_ui_adapter_preview_ready() noexcept;
bool forum_media_ui_adapter_reveal_narrow(u64 draft_id) noexcept;
bool forum_media_ui_adapter_child() noexcept;
UiWidgetManager* forum_media_ui_adapter_focus() noexcept;
bool forum_media_ui_adapter_description_saved(u64, Utf8View) noexcept;
} // namespace maicord

#pragma once
// Shared by requests.cpp and its parts (requests_*.cpp); private to them.
#include "../shell/load_trace.hpp"
#include "../shell/log_fields.hpp"
#include "pump.hpp"
#include "../chat/channel_authority.hpp"
#include "../account/profile.hpp"
#include "../account/profile_editor.hpp"
#include "../guild/card.hpp"
#include "expression_card.hpp"
#include "maicord/ui/media/expression_card_scene.hpp"
#include "../guild/banner.hpp"
#include "picker.hpp"
#include "../chat/composer/preview.hpp"
#include "image_viewer.hpp"
#include "catalog_preview_log.hpp"
#include "maicord/render/media/inline_media.hpp"
#include "maicord/platform/desktop/clipboard_image.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/core/animation_timeline.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/chat/search/avatar_media.hpp"
#include "../chat/search/search.hpp"
#include "../voice/stream.hpp"
#include "maicord/ui/chat/pinned_messages_avatar_media.hpp"
#include "maicord/ui/chat/threads/forum_channel_media.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/quick_switcher_scene.hpp"
#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "maicord/ui/chat/inbox_avatar_media.hpp"
#include "maicord/ui/chat/polls/voters_avatar_media.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace requests_detail {}
using namespace requests_detail;
namespace requests_detail {

bool preview_media_allowed(const Application& application, const MessageRecord& message,
                           const MessageRichStore& store, bool search, MessageMediaSource source, u32 index,
                           bool snapshot, bool spoiler, bool explicit_media) noexcept;
bool observe_message_animation(Application& application, const MediaAssetKey& key, u64 message_id) noexcept;
bool forum_thumbnail_key_current(const Application& application, const MediaAssetKey& key) noexcept;
MediaAttachmentFormat attachment_format_for_key(const MediaAssetKey& key, const MessageRichStore& store,
                                                const MessageRichAttachment& item) noexcept;

enum class FrameTake : u8 { failed, stale, deferred, uploaded };

__declspec(noinline) FrameTake take_ready_frame(ApplicationMediaPump& pump, const MediaAssetKey& key,
                                                bool message_owned, bool stale_fails,
                                                AccountMediaFrame* frame = nullptr) noexcept;
__declspec(noinline) void note_frame_uploaded(ApplicationMediaPump& pump) noexcept;
__declspec(noinline) bool message_media_resident(ApplicationMediaPump& pump, const MessageRecord& message,
                                                 const MediaAssetKey& key, bool animated_hint,
                                                 bool static_resident) noexcept;

} // namespace requests_detail
} // namespace maicord::app_detail

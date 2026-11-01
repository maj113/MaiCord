#pragma once
// Shared by requests.cpp and its parts (requests_*.cpp); private to them.
#include "pump.hpp"
#include "../chat/channel_authority.hpp"
#include "picker.hpp"
#include "../chat/composer/preview.hpp"
#include "image_viewer.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/core/animation_timeline.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "../chat/search/search.hpp"
#include "maicord/ui/shell/quick_switcher_scene.hpp"
#include "maicord/ui/chat/polls/voters_avatar_media.hpp"

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

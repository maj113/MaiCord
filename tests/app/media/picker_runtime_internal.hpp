#pragma once

#include "../../../src/app/media/picker_internal.hpp"

namespace maicord::media_picker_runtime_test {
using namespace app_detail;

extern u32 failures, text_builds;

extern u32 preference_saves;

extern u32 member_activations, launcher_activations;

extern bool settings;

extern bool favorites_enabled, favorites_visible;

extern u32 favorite_toggles;

extern Utf8View toggled_url, toggled_src;

extern GifFavoriteFormat toggled_format;

extern u32 reaction_calls;

extern u64 reaction_messages[4];

extern Utf8View reaction_names[4];

extern app_detail::ApplicationMessageReactionEnqueueResult reaction_next;

extern ApplicationGifFavoritesView favorites;

extern ApplicationGifFavoriteState favorite_state;

} // namespace maicord::media_picker_runtime_test

namespace maicord {
namespace app_detail {

Utf8View application_message_reaction_admission_text(ApplicationMessageReactionEnqueueResult admission) noexcept;

} // namespace app_detail
} // namespace maicord

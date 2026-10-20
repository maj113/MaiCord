#pragma once

#include "application_account_presentation.hpp"
#include "application_state.hpp"
#include "application_user_settings.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord::app_detail {
// True consumes this activation before an original, viewer or player can open.
// Revealing and opening are separate gestures, including keyboard activation.
__declspec(noinline) inline bool application_media_activation_consumed(MessageScene& scene,
                                                                       const MessageMediaOwner& owner,
                                                                       u64 plugin_message_id, u64 key,
                                                                       bool spoiler,
                                                                       bool explicit_media) noexcept {
    const auto policy = message_scene_media_policy(scene, owner);
    const auto obscure =
        message_scene_media_obscure(scene, policy, plugin_message_id, key, spoiler, explicit_media);
    if (obscure == MessageMediaObscure::none)
        return false;
    if (message_media_obscure_notice(obscure))
        return true;
    if (obscure == MessageMediaObscure::explicit_content && explicit_media &&
        policy.filter != MessageExplicitFilter::show && !policy.explicit_reveal_allowed) {
        application_user_settings_request_content_confirmation();
        return true;
    }
    if (message_scene_reveal_media(&scene, key)) {
        if (&scene == &g_app.message_scene)
            refresh_account_frame(true);
        else
            application_request_frame();
    }
    return true;
}
} // namespace maicord::app_detail

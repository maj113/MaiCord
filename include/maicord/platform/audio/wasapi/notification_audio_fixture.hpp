#pragma once

#include "maicord/platform/audio/notification_audio.hpp"
#include "maicord/sys/wasapi.hpp"

// The WASAPI test seam of the Windows notification audio worker (MAICORD_NOTIFICATION_AUDIO_FIXTURE builds only;
// the shipping library excludes it completely).
//
// Install the hooks before the audio object is created; keep the hooks/context unchanged until every worker has
// joined and the callback reference is released. With no hook installed, COM creation fails; a real endpoint is
// never opened.
namespace maicord {

struct NotificationAudioFixtureHooks {
    void* context{};
    HRESULT (*initialize)(void*) noexcept;
    void (*uninitialize)(void*) noexcept;
    HRESULT (*create)(void*, IMMDeviceEnumerator**) noexcept;
    // Stands for window_post(config.wake, 0, 0): true when the wake was posted.
    bool (*post_wake)(void*, const Waker&) noexcept;
    void (*before_wait)(void*, u32 handle_count) noexcept;
};
void notification_audio_fixture_hooks(const NotificationAudioFixtureHooks* hooks) noexcept;
// For inspection only, while the caller still holds a fixture callback reference.
HANDLE notification_audio_fixture_callback_event(IMMNotificationClient* callback) noexcept;

} // namespace maicord

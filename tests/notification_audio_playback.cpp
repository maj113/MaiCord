#include "maicord/app/notification_sound_assets.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"

// Opt-in hardware check: the shipping asset resolver, gain and WASAPI worker.
// Completion counts prove delivery to the endpoint; audibility is a manual check.
extern "C" __declspec(noreturn) void WINAPI maicord_notification_audio_playback_entry() noexcept {
    using namespace maicord;
    NotificationSoundAssetBank bank{};
    NotificationAudio owner{};
    const NotificationAudioConfig config{notification_sound_asset_resolve, &bank, nullptr, 0, 1, true,
                                         notification_sound_asset_gain};
    bool ok = notification_audio_initialize(&owner, config).ok() &&
              notification_audio_submit(&owner, {1, 1, .4f, {}}).ok();
    NotificationAudioStatus status{};
    const u64 deadline = GetTickCount64() + 2000;
    while (ok) {
        status = notification_audio_status(&owner);
        if (status.completed || status.failed || status.worker_exited || GetTickCount64() >= deadline)
            break;
        Sleep(1);
    }
    test_write_metric("audio_accepted=", status.accepted);
    test_write_metric("audio_completed=", status.completed);
    test_write_metric("audio_failed=", status.failed);
    test_write_metric("audio_error=", static_cast<u32>(status.error));
    test_write_metric("audio_native_error=", status.native_error);
    ok = ok && status.accepted == 1 && status.completed == 1 && !status.failed && !status.canceled;
    const auto joined = notification_audio_release(&owner, 2000);
    if (joined.ok()) notification_sound_assets_release(&bank);
    ExitProcess(ok && joined.ok() ? 0 : 1);
}

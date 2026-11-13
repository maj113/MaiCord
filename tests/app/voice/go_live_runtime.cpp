// Go Live's snapshot polling (src/app/voice/go_live.cpp) over the account fake: the client reads the account's stream
// snapshot on a timer only while a stream starts or runs, so an account whose snapshot cannot be read (no Gateway) or
// that is not streaming asks the loop for no wake.
#include "../../support/account_module_fake.hpp"
#include "../../../src/app/voice/go_live.cpp"
#include "../../support/test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::go_live_runtime_test {
using namespace app_detail;
u32 failures;
bool readable;
GoLiveSnapshot answer;
u8 instance;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool snapshot(void*, GoLiveSnapshot* output) noexcept {
    if (readable)
        *output = answer;
    return readable;
}
// The account answers `state` as a new generation; the next pump reads it.
void answer_next(GoLiveState state) noexcept {
    answer.state = state;
    ++answer.generation;
    ui.polled_ms = 0;
    application_go_live_pump();
}

void run() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "go live: the account UI fixture allocates");
    if (!g_app.account_ui)
        return;
    g_app.account.instance = &instance;
    g_account_fake.go_live_snapshot = snapshot;
    // Without Nitro or boosts the smoother preset stops at 720p 30fps and a custom 1440p 60fps falls back; unlocked,
    // every quality is offered.
    auto& p = prefs();
    const VoicePreferences saved = p;
    p.stream_preset = static_cast<u8>(StreamPreset::video);
    const StreamQuality locked = resolved();
    p.stream_unlocked = true;
    const StreamQuality unlocked = resolved();
    p.stream_preset = static_cast<u8>(StreamPreset::custom);
    p.stream_resolution = 1440;
    p.stream_fps = 60;
    const StreamQuality custom_unlocked = resolved();
    p.stream_unlocked = false;
    const StreamQuality custom_locked = resolved();
    p = saved;
    check(locked.resolution == 720 && locked.fps == 30 && unlocked.resolution == 1440 && unlocked.fps == 60 &&
              custom_unlocked.resolution == 1440 && custom_unlocked.fps == 60 && custom_locked.resolution != 1440,
          "go live: unlocking all qualities offers 1440p 60fps without Nitro or boosts");

    application_go_live_pump();
    check(!ui.snapshot_valid && application_go_live_next_wake() == 0,
          "go live: an account that cannot give its snapshot asks for no wake");
    readable = true;
    answer_next(GoLiveState::starting);
    check(application_go_live_next_wake() == ui.polled_ms + kStartingPollMs,
          "go live: a starting stream is polled every 100 ms");
    answer_next(GoLiveState::live);
    check(application_go_live_next_wake() == ui.polled_ms + kLivePollMs, "go live: a live stream is polled every 500 ms");
    answer_next(GoLiveState::idle);
    check(application_go_live_next_wake() == 0, "go live: an idle account asks for no wake");

    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    application_go_live_pump();
    check(!ui.snapshot_valid && application_go_live_next_wake() == 0, "go live: without an account nothing wakes");
    g_account_fake.go_live_snapshot = nullptr;
    g_app.account.instance = nullptr;
}
} // namespace maicord::go_live_runtime_test

// The host this fixture leaves out: no call, popup, dialog or media upload is ever opened.
namespace maicord {
MediaAtlasGpuResult renderer_upload_media_straight_rgba8(D3D11Renderer*, MediaAssetKey, u16, u16, u32,
                                                         ConstSlice<u8>) noexcept {
    return {};
}
bool media_atlas_forget(MediaAtlas*, MediaAssetKey) noexcept {
    return false;
}
namespace app_detail {
const GatewayClientVoiceSnapshot* application_voice_call() noexcept {
    return nullptr;
}
VoicePreferences& application_voice_preferences() noexcept {
    static VoicePreferences preferences{};
    return preferences;
}
void application_voice_settings_changed(bool) noexcept {}
UiRect application_viewport_dips() noexcept {
    return {};
}
void set_popup_owner(PopupOwnerKind) noexcept {}
bool refresh_popup_text() noexcept {
    return true;
}
bool dismiss_popup() noexcept {
    return true;
}
bool handle_popup_hover(const WindowEvent&) noexcept {
    return false;
}
bool handle_popup_click(const WindowEvent&) noexcept {
    return false;
}
} // namespace app_detail
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_go_live_runtime_entry) {
    maicord::go_live_runtime_test::run();
    maicord::test_exit(maicord::go_live_runtime_test::failures, "Go Live runtime");
}

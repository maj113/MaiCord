#include "notice.hpp"
#include "state.hpp"
#include "surface_motion.hpp"
#include "../account/account_presentation.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../settings/streamer_mode.hpp"
#include "../settings/settings.hpp"
#include "../settings/test_mode.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kNoticeMotionOwner = 0x4e4f'5449'4345'4231ull; // "NOTICEB1"
struct Notice {
    UiNoticeBand band;
    ApplicationNoticeSource source;
    // What the shown text was built from: a change rebuilds it.
    u64 signature;
    bool streamer_dismissed;
    UiMotionTransition expand;
    // The laid-out height: the shell lays out again only when it changes by a whole DIP.
    float laid_out;
    char text[320];
};
Notice g_notice{.source = ApplicationNoticeSource::count};

// The highest-priority notice that applies (ui_notice_band_select); count when none does.
ApplicationNoticeSource choose(u64* signature) noexcept {
    const bool streamer = application_streamer_mode_active();
    if (!streamer)
        g_notice.streamer_dismissed = false;
    // In ApplicationNoticeSource order: the official client's priority.
    const UiNoticeRequest requests[]{{0, streamer, g_notice.streamer_dismissed},
                                     {1, g_app.preferences.test_mode_application != 0, false}};
    const auto source = static_cast<ApplicationNoticeSource>(ui_notice_band_select({requests, 2}));
    *signature = 0;
    if (source == ApplicationNoticeSource::streamer_mode)
        *signature = 1;
    else if (source == ApplicationNoticeSource::test_mode) {
        const Utf8View name = application_test_mode_name();
        *signature = hash64_bytes(g_app.preferences.test_mode_application, name.data, name.bytes) | 2;
    }
    return source;
}

void build(ApplicationNoticeSource source) noexcept {
    auto& band = g_notice.band;
    const float height = band.height;
    reset_value(&band);
    band.dismissible = true;
    band.height = height;
    if (source == ApplicationNoticeSource::streamer_mode) {
        band.kind = UiNoticeKind::brand;
        band.text = utf8_literal("Streamer Mode is enabled.");
        band.action = utf8_literal("Disable");
        return;
    }
    // "Test mode is currently active for **{application}**. Unpublished SKUs ...": the settings row's text.
    const Utf8View status = application_test_mode_status();
    band.kind = UiNoticeKind::warning;
    band.text = {g_notice.text, utf8_copy_truncated(g_notice.text, sizeof(g_notice.text), status)};
    band.bold[0] = {kApplicationTestModeActiveHead.bytes, application_test_mode_name().bytes, text_style_bold};
    band.bold_count = 1;
}

bool relayout() noexcept {
    return synchronize_shell_layout() && refresh_account_text(true) && application_request_frame();
}
} // namespace

void application_notice_synchronize(u64 now_ms) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return;
    u64 signature = 0;
    const auto source = choose(&signature);
    const bool shown = source != ApplicationNoticeSource::count;
    if (source != g_notice.source || signature != g_notice.signature) {
        // A notice replacing another keeps the band open and swaps its content; none collapses the last one.
        if (shown)
            build(source);
        g_notice.source = source;
        g_notice.signature = signature;
    }
    if (!g_notice.expand.scope.owner)
        ui_motion_transition_reset(&g_notice.expand, {kNoticeMotionOwner, 1}, 0);
    // The official bar appears at full height; it expands here (kUiMotionExpand), at once with reduced motion.
    const float value = application_motion_tick(&g_notice.expand, g_notice.expand.scope, shown ? 1.f : 0.f, now_ms,
                                                kUiMotionExpand);
    g_notice.band.height = static_cast<float>(static_cast<u32>(value * kUiNoticeBandHeight + .5f));
    // The shell scene draws the band over its root's top inset; the layout reads its height
    // (account_presentation.cpp).
    g_app.shell_scene.notice = application_notice_band();
    if (g_notice.band.height != g_notice.laid_out) {
        g_notice.laid_out = g_notice.band.height;
        if (!relayout())
            log_write(LogLevel::warning, "notice", "layout_failed");
    }
}

const UiNoticeBand* application_notice_band() noexcept {
    return g_notice.band.height > 0 && g_notice.band.text.bytes ? &g_notice.band : nullptr;
}

bool application_notice_activate(UiNoticeBandPart part) noexcept {
    if (!application_notice_band() || g_notice.source == ApplicationNoticeSource::count)
        return false;
    // Disable turns the switch off; Streamer Mode a streaming tool turned on stays while it runs, so its notice hides.
    if (part == UiNoticeBandPart::action && g_notice.source == ApplicationNoticeSource::streamer_mode) {
        if (g_app.preferences.streamer_mode)
            return application_streamer_mode_toggle() || true;
        part = UiNoticeBandPart::dismiss;
    }
    if (part != UiNoticeBandPart::dismiss)
        return part == UiNoticeBandPart::band;
    if (g_notice.source == ApplicationNoticeSource::streamer_mode)
        g_notice.streamer_dismissed = true;
    else
        application_test_mode_clear();
    application_request_frame();
    return true;
}

bool application_notice_click(float x, float y) noexcept {
    // Settings and dialogs cover the band.
    const auto* band = application_settings_active() || g_app.text_form ? nullptr : application_notice_band();
    const auto* root = band ? ui_scene_node(g_app.shell_scene, UiSemantic::root) : nullptr;
    if (!root)
        return false;
    const auto g = ui_notice_band_geometry(*band, root->rectangle,
                                           g_app.account_ui ? &g_app.account_ui->text_measure_cache : nullptr);
    const auto part = ui_notice_band_hit(g, x, y);
    return part != UiNoticeBandPart::none && application_notice_activate(part);
}
} // namespace maicord::app_detail

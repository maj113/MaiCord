#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_state.hpp"
#include "application_composer.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/message_composer.hpp"

// Composer integration for ABI-5 plugins: local slash commands and the
// pre-send transform run before application commands and the ordinary send.
namespace maicord::app_detail {
namespace {
constexpr u64 kComposerNoticeMs = 8000;

u64 channel_guild(u64 channel) noexcept {
    const auto* record = g_app.account_ui && channel ?
        chat_state_store_find_channel(g_app.account_ui->chat_state, channel) : nullptr;
    return record ? record->guild_id : 0;
}
void set_composer_notice(ApplicationPluginsSdk& sdk, Utf8View text) noexcept {
    const u32 bytes = utf8_copy_truncated(sdk.composer_notice, text);
    sdk.composer_notice_bytes = bytes;
    sdk.composer_notice_until = bytes ? clock_monotonic_milliseconds() + kComposerNoticeMs : 0;
    sdk.composer_notice_tone = 0;
    sdk.composer_refresh = refresh_message_composer_ui;
}
} // namespace

bool application_plugins_compose(bool* handled) noexcept {
    *handled = false;
    auto* sdk = g_app.plugins ? g_app.plugins->sdk : nullptr;
    if (!sdk || !g_app.account_ui || g_app.message_edit.editor_active) return true;
    auto& composer = g_app.message_composer;
    const Utf8View text = message_composer_text(composer);
    const NativePluginSdkDraft draft{composer.channel_id, channel_guild(composer.channel_id),
                                    g_app.account_ui->chat_state.self_user_id, text};
    auto result = native_plugin_sdk_run_command(&sdk->runtime, draft);
    if (result.action == NativePluginSdkComposeAction::none) {
        // Other slash text belongs to application commands; transforms see messages only.
        if (!text.bytes || text.data[0] == '/') return true;
        result = native_plugin_sdk_pre_send(&sdk->runtime, draft);
        if (result.action == NativePluginSdkComposeAction::cancel) {
            *handled = true;
            set_composer_notice(*sdk, utf8_literal("A plugin stopped this message from being sent."));
            return true;
        }
        if (result.action != NativePluginSdkComposeAction::replace) return true;
        if (!result.text.bytes) {
            *handled = true;
            return true;
        }
        return message_composer_replace_range(&composer, 0, composer.bytes, result.text) !=
               MessageComposerResult::invalid_input;
    }
    switch (result.action) {
    case NativePluginSdkComposeAction::send:
        // The normal send path sends the command's text.
        return message_composer_replace_range(&composer, 0, composer.bytes, result.text) !=
               MessageComposerResult::invalid_input;
    case NativePluginSdkComposeAction::replace:
        *handled = true;
        return composer_apply_result(message_composer_replace_range(&composer, 0, composer.bytes, result.text), true);
    case NativePluginSdkComposeAction::notice:
        *handled = true;
        set_composer_notice(*sdk, result.text);
        return true;
    default:
        *handled = true;
        return composer_apply_result(message_composer_replace_range(&composer, 0, composer.bytes, {}), true);
    }
}

} // namespace maicord::app_detail

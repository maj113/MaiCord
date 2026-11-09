#include "maicord/core/base.hpp"

// The plugin pipeline's application hooks (src/app/plugins/pipeline_host.cpp) for tests of code that calls them
// but never runs a plugin: no plugin suppresses typing or embeds. Sends never come from /tts either
// (application_commands_take_tts, src/app/chat/composer/commands.cpp).
namespace maicord::app_detail {

bool application_plugins_take_suppress_embeds() noexcept {
    return false;
}
bool application_plugins_typing_suppressed(u64) noexcept {
    return false;
}
bool application_commands_take_tts() noexcept {
    return false;
}

} // namespace maicord::app_detail

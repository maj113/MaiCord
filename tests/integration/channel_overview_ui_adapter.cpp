// This offline fixture publishes immutable Settings editor projections. The
// application owner and native target lifetimes are exercised separately by
// application_channel_overview_runtime.cpp; no account owner is present here.
#include "../../src/app/application_channel_overview.hpp"
namespace maicord::app_detail {
bool application_channel_overview_edit_focused() noexcept { return false; }
MessageComposer* application_channel_overview_edit_model() noexcept { return nullptr; }
TextInputTarget application_channel_overview_edit_target() noexcept { return {}; }
UiRect application_channel_overview_edit_bounds() noexcept { return {}; }
bool application_channel_overview_edit_changed(bool) noexcept { return false; }
void application_channel_overview_edit_cancel_native_group(TextInputTarget) noexcept {}
}

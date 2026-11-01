#pragma once
#include "maicord/app/plugins/consent.hpp"

namespace maicord::app_detail {

// The user allowed the plugin: approval is everything the dialog listed. The caller remembers it
// (plugin_approval_store) and enables the plugin with it.
using PluginConsentAccepted = void (*)(void* context, const PluginApproval& approval) noexcept;

// Opens the consent dialog for request (plugin_consent_text, the high-risk parts with the danger
// button). False when another consent dialog is open or the form cannot open. The request's text
// is copied; its hosts need not outlive the call.
bool application_plugin_consent_open(const PluginConsentRequest& request, PluginConsentAccepted accepted,
                                     void* context) noexcept;

} // namespace maicord::app_detail

#pragma once

#include "maicord/app/plugins/ui.hpp"

// The application's side of the plugin UI (the plugin_host_* functions of maicord/app/plugins/ui.hpp) for the tests:
// they link fake_ui_host.cpp in place of src/app/plugins/ui_host.cpp and read what the runtime asked to show. An
// application that shows nothing: modals answer busy until accept_modal is set.
namespace maicord::plugin_fake_ui {

struct Recorded {
    u32 banner_changes, modal_opens, modal_closes, urls, copies, channels, profiles, notices, toasts;
    bool accept_modal;
    PluginUiStatus status; // the answer of open_url, copy_text, open_channel and open_profile
    // The last modal as it was asked for, and its text (the request's own views end with the call).
    PluginModalRequest modal;
    char title[64], context[600], accept[40], cancel[40], input_label[80], input_initial[2100], hint[80], toggle[100],
        select_label[80];
    char url[2100], copy[4100];
    u64 channel, user, guild;
    u64 notice_channel;
    char notice[200];
    u64 toast_owner;
    char toast[200];
    u32 toast_tone;
};
extern Recorded recorded;

} // namespace maicord::plugin_fake_ui

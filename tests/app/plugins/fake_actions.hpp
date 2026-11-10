#pragma once

#include "maicord/app/plugins/pipeline.hpp"

// What the application does for a plugin (the plugin_host_* functions of maicord/app/plugins/pipeline.hpp) for the
// tests: they link fake_actions.cpp in place of src/app/plugins/pipeline_host.cpp. Every action is counted and answers
// `result`; the open channel is 9 (server 4, message 31) and holds the messages 13, 12 and 11.
namespace maicord::plugin_fake_actions {

struct Recorded {
    u32 sends, edits, reacts, deletes, reads, activities, cleared, locals, local_removes, composer, keep_changed;
    u64 owner, channel, message;
    u32 flags;
    char text[16];      // the last send's text, when it fits
    char inserted[128]; // the last text inserted into the composer, when it fits
    u32 inserted_bytes;
    char local[32];     // the last local message's text, when it fits
    u32 local_bytes;
    mc_status result;
};
extern Recorded recorded;

} // namespace maicord::plugin_fake_actions

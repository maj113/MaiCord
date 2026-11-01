/* What the surfaces fixture plugin (plugin_surfaces_fixture.c) records in its host-owned state, for the tests
   and the UI probe to read through ctx->state. Plain C, no includes beyond stdint. */
#ifndef MAICORD_PLUGIN_SURFACES_FIXTURE_H
#define MAICORD_PLUGIN_SURFACES_FIXTURE_H

#include <stdint.h>

#define SURFACES_OWNER 0x4D43535552464353ull

/* Descriptor button indices. */
enum {
    SURFACES_BUTTON_CHAT_TOGGLE = 0, /* chat bar toggle; a click flips ACTIVE and a badge through button_state */
    SURFACES_BUTTON_POPOVER_A = 1,   /* shown for messages whose content starts with 'a' */
    SURFACES_BUTTON_POPOVER_B = 2,   /* no visible function: every message */
    SURFACES_BUTTON_POPOVER_C = 3,   /* shown for content starting with 'c'; a click pushes a decoration */
    SURFACES_BUTTON_ACCOUNT = 4,
    SURFACES_BUTTON_HEADER = 5,
    SURFACES_BUTTON_SERVER_TOP = 6,
    SURFACES_BUTTON_SERVER_BOTTOM = 7,
    SURFACES_BUTTON_HIDDEN = 8, /* visible returns 0 */
    SURFACES_BUTTON_COUNT = 9
};

typedef struct surfaces_state {
    uint32_t message_calls, user_calls, channel_calls, guild_calls, header_calls;
    uint32_t visible_calls, click_calls, action_calls;
    uint32_t last_button, last_action_kind, last_action_source, toggled;
    uint64_t last_click_message, last_click_channel, last_click_guild;
    uint64_t last_action_tag, last_action_message, last_action_user, last_action_channel;
    int32_t push_status, state_status, start_status, reserved;
    uint32_t last_click_flags;  /* ctx->flags during the last click */
    uint32_t last_action_flags; /* ctx->flags during the last node action */
    uint32_t last_click_content_bytes;
    uint32_t reserved2;
} surfaces_state;

#endif

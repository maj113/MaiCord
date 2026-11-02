/* A plain C11 plugin against maicord/sdk/maicord_plugin.h: the C path of ABI 6
   and the frozen layouts. Its pre_send prefixes "[c] " and cancels "cancel";
   its message decoration adds a "C" badge; /shout inserts its text in upper
   case; a "C tools" submenu on links holds "Count"; a chat-bar button
   counts clicks; start and stop count in the state block. */
#include "maicord/sdk/maicord_plugin.h"

#define OFFSET(type, field, at) (offsetof(type, field) == (at))
_Static_assert(sizeof(mc_str) == 16 && OFFSET(mc_str, bytes, 8), "mc_str");
_Static_assert(sizeof(mc_list) == 16 && OFFSET(mc_list, count, 8), "mc_list");
_Static_assert(sizeof(mc_ids) == 16 && OFFSET(mc_ids, count, 8), "mc_ids");
_Static_assert(sizeof(mc_ctx) == 32 && OFFSET(mc_ctx, settings, 16) && OFFSET(mc_ctx, flags, 24) &&
                   OFFSET(mc_ctx, call, 28),
               "mc_ctx");
_Static_assert(sizeof(mc_setting_def) == 128 && OFFSET(mc_setting_def, default_text, 64) &&
                   OFFSET(mc_setting_def, items, 80) && OFFSET(mc_setting_def, item_count, 88) &&
                   OFFSET(mc_setting_def, type, 92) && OFFSET(mc_setting_def, offset, 96) &&
                   OFFSET(mc_setting_def, minimum, 104) && OFFSET(mc_setting_def, default_int, 116) &&
                   OFFSET(mc_setting_def, visible_if, 120) && OFFSET(mc_setting_def, max_bytes, 122) &&
                   OFFSET(mc_setting_def, visible_value, 124),
               "mc_setting_def");
_Static_assert(sizeof(mc_hook) == 24 && OFFSET(mc_hook, filter, 8) && OFFSET(mc_hook, fn, 16), "mc_hook");
/* Views */
_Static_assert(sizeof(mc_message) == 112 && OFFSET(mc_message, timestamp_ms, 40) &&
                   OFFSET(mc_message, content, 56) && OFFSET(mc_message, author_name, 72) &&
                   OFFSET(mc_message, flags, 88) && OFFSET(mc_message, revision, 96) &&
                   OFFSET(mc_message, embed_count, 108),
               "mc_message");
_Static_assert(sizeof(mc_user) == 80 && OFFSET(mc_user, where, 24) && OFFSET(mc_user, platforms, 40) &&
                   OFFSET(mc_user, name, 48) && OFFSET(mc_user, username, 64),
               "mc_user");
_Static_assert(sizeof(mc_channel) == 64 && OFFSET(mc_channel, self_permissions, 24) &&
                   OFFSET(mc_channel, type, 32) && OFFSET(mc_channel, name, 48),
               "mc_channel");
_Static_assert(sizeof(mc_guild) == 56 && OFFSET(mc_guild, flags, 24) && OFFSET(mc_guild, online_count, 36) &&
                   OFFSET(mc_guild, name, 40),
               "mc_guild");
_Static_assert(sizeof(mc_header) == 48 && OFFSET(mc_header, where, 16) && OFFSET(mc_header, name, 32), "mc_header");
/* Message pipeline */
_Static_assert(sizeof(mc_draft) == 72 && OFFSET(mc_draft, text, 32) && OFFSET(mc_draft, buffer, 48) &&
                   OFFSET(mc_draft, buffer_capacity, 56) && OFFSET(mc_draft, replaced_bytes, 60) &&
                   OFFSET(mc_draft, flags, 64) && OFFSET(mc_draft, verdict, 68),
               "mc_draft");
_Static_assert(sizeof(mc_click) == 16 && OFFSET(mc_click, modifiers, 8), "mc_click");
_Static_assert(sizeof(mc_kept_info) == 32 && OFFSET(mc_kept_info, author_id, 8), "mc_kept_info");
_Static_assert(sizeof(mc_kept) == 88 && OFFSET(mc_kept, content, 48) && OFFSET(mc_kept, edits, 80), "mc_kept");
_Static_assert(sizeof(mc_event) == 88 && OFFSET(mc_event, value, 40) && OFFSET(mc_event, time_ms, 48) &&
                   OFFSET(mc_event, text, 56) && OFFSET(mc_event, name, 72),
               "mc_event");
/* Decorations and buttons */
_Static_assert(sizeof(mc_node) == 80 && OFFSET(mc_node, text, 8) && OFFSET(mc_node, value, 56) &&
                   OFFSET(mc_node, tag, 72),
               "mc_node");
_Static_assert(sizeof(mc_card_button) == 32 && OFFSET(mc_card_button, tag, 16), "mc_card_button");
_Static_assert(sizeof(mc_card) == 152 && OFFSET(mc_card, thumbnail, 64) && OFFSET(mc_card, buttons, 80) &&
                   OFFSET(mc_card, button_count, 144) && OFFSET(mc_card, tone, 148),
               "mc_card");
_Static_assert(sizeof(mc_decoration) == 1208 && OFFSET(mc_decoration, collapsed_label, 16) &&
                   OFFSET(mc_decoration, content, 64) && OFFSET(mc_decoration, badge_count, 80) &&
                   OFFSET(mc_decoration, badges, 96) && OFFSET(mc_decoration, accessories, 416) &&
                   OFFSET(mc_decoration, card, 1056),
               "mc_decoration");
_Static_assert(sizeof(mc_node_action) == 48 && OFFSET(mc_node_action, source, 40), "mc_node_action");
_Static_assert(sizeof(mc_button_event) == 32 && OFFSET(mc_button_event, message, 16) &&
                   OFFSET(mc_button_event, button, 24),
               "mc_button_event");
_Static_assert(sizeof(mc_button_def) == 56 && OFFSET(mc_button_def, icon, 24) &&
                   OFFSET(mc_button_def, visible, 40) && OFFSET(mc_button_def, click, 48),
               "mc_button_def");
_Static_assert(sizeof(mc_button_state) == 40 && OFFSET(mc_button_state, icon, 24), "mc_button_state");
/* Menus and commands */
_Static_assert(sizeof(mc_menu_target) == 112 && OFFSET(mc_menu_target, item_id, 40) &&
                   OFFSET(mc_menu_target, url, 48) && OFFSET(mc_menu_target, attachment_index, 96),
               "mc_menu_target");
_Static_assert(sizeof(mc_menu_def) == 48 && OFFSET(mc_menu_def, targets, 16) && OFFSET(mc_menu_def, parent, 24) &&
                   OFFSET(mc_menu_def, visible, 32) && OFFSET(mc_menu_def, select, 40),
               "mc_menu_def");
_Static_assert(sizeof(mc_arg_def) == 72 && OFFSET(mc_arg_def, minimum, 40) && OFFSET(mc_arg_def, choices, 56),
               "mc_arg_def");
_Static_assert(sizeof(mc_arg) == 40 && OFFSET(mc_arg, integer, 24) && OFFSET(mc_arg, id, 32), "mc_arg");
_Static_assert(sizeof(mc_command_call) == 56 && OFFSET(mc_command_call, reply, 32) &&
                   OFFSET(mc_command_call, reply_tone, 48),
               "mc_command_call");
_Static_assert(sizeof(mc_command_def) == 56 && OFFSET(mc_command_def, args, 32) && OFFSET(mc_command_def, run, 48),
               "mc_command_def");
/* Network, feedback, completions, actions */
_Static_assert(sizeof(mc_http_request) == 56 && OFFSET(mc_http_request, headers, 24) &&
                   OFFSET(mc_http_request, body, 40) && OFFSET(mc_http_request, max_response_bytes, 52),
               "mc_http_request");
_Static_assert(sizeof(mc_banner) == 64 && OFFSET(mc_banner, title, 16), "mc_banner");
_Static_assert(sizeof(mc_modal) == 176 && OFFSET(mc_modal, input_max_chars, 120) &&
                   OFFSET(mc_modal, toggle_label, 128) && OFFSET(mc_modal, select_items, 160),
               "mc_modal");
_Static_assert(sizeof(mc_completion) == 64 && OFFSET(mc_completion, body, 24) &&
                   OFFSET(mc_completion, result, 32) && OFFSET(mc_completion, text, 48),
               "mc_completion");
_Static_assert(sizeof(mc_send) == 40 && OFFSET(mc_send, text, 16) && OFFSET(mc_send, flags, 32), "mc_send");
_Static_assert(sizeof(mc_activity) == 112 && OFFSET(mc_activity, name, 32) && OFFSET(mc_activity, emoji, 96),
               "mc_activity");
_Static_assert(sizeof(mc_descriptor) == 232 && OFFSET(mc_descriptor, owner_id, 16) &&
                   OFFSET(mc_descriptor, name, 24) && OFFSET(mc_descriptor, permissions, 104) &&
                   OFFSET(mc_descriptor, network, 112) && OFFSET(mc_descriptor, state_bytes, 124) &&
                   OFFSET(mc_descriptor, settings_bytes, 132) && OFFSET(mc_descriptor, state_layout, 136) &&
                   OFFSET(mc_descriptor, settings, 144) && OFFSET(mc_descriptor, hook_count, 156) &&
                   OFFSET(mc_descriptor, hooks, 160) && OFFSET(mc_descriptor, commands, 168) &&
                   OFFSET(mc_descriptor, menus, 184) && OFFSET(mc_descriptor, buttons, 192) &&
                   OFFSET(mc_descriptor, theme, 208) && OFFSET(mc_descriptor, start, 216) &&
                   OFFSET(mc_descriptor, stop, 224),
               "mc_descriptor");
_Static_assert(OFFSET(mc_api, log, 8) && OFFSET(mc_api, set_setting, 16) && OFFSET(mc_api, store_get, 24) &&
                   OFFSET(mc_api, store_set, 32) && OFFSET(mc_api, store_remove, 40) && OFFSET(mc_api, toast, 48) &&
                   OFFSET(mc_api, invalidate, 56) && OFFSET(mc_api, read_permissions, 104) &&
                   OFFSET(mc_api, banner, 112) && OFFSET(mc_api, open, 152) && OFFSET(mc_api, http, 160) &&
                   OFFSET(mc_api, local_message, 168) && OFFSET(mc_api, composer_edit, 224) &&
                   OFFSET(mc_api, send_message, 232) && OFFSET(mc_api, set_activity, 272) &&
                   OFFSET(mc_api, timer, 280) && OFFSET(mc_api, keep_restore, 312) && sizeof(mc_api) == 320,
               "mc_api level 1");

typedef struct state_block {
    uint32_t starts;
    uint32_t stops;
    uint32_t counted;
    uint32_t clicks;
} state_block;

static int is_cancel(mc_str text) {
    static const char word[] = "cancel";
    if (text.bytes != sizeof(word) - 1u)
        return 0;
    for (uint32_t i = 0; i < text.bytes; ++i)
        if (text.data[i] != word[i])
            return 0;
    return 1;
}

static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    static const char prefix[] = "[c] ";
    const uint32_t prefix_bytes = sizeof(prefix) - 1u;
    (void)ctx;
    if (is_cancel(draft->text)) {
        draft->verdict = MC_DRAFT_CANCEL;
        return MC_OK;
    }
    if (draft->text.bytes > draft->buffer_capacity - prefix_bytes)
        return MC_E_CAPACITY;
    for (uint32_t i = 0; i < prefix_bytes; ++i)
        draft->buffer[i] = prefix[i];
    for (uint32_t i = 0; i < draft->text.bytes; ++i)
        draft->buffer[prefix_bytes + i] = draft->text.data[i];
    draft->replaced_bytes = prefix_bytes + draft->text.bytes;
    draft->verdict = MC_DRAFT_REPLACE;
    return MC_OK;
}

static mc_status MC_CALL decorate(mc_ctx* ctx, const mc_message* message, mc_decoration* out) {
    static const char badge[] = "C";
    (void)ctx;
    if (!message->content.bytes)
        return MC_OK;
    out->badges[0].kind = MC_NODE_BADGE;
    out->badges[0].tone = MC_TONE_ACCENT;
    out->badges[0].text.data = badge;
    out->badges[0].text.bytes = 1u;
    out->badge_count = 1;
    return MC_OK;
}

/* /shout text: inserts the text in upper case. */
static mc_status MC_CALL shout(mc_ctx* ctx, mc_command_call* call) {
    (void)ctx;
    if (call->arg_count != 1 || !call->args[0].present || call->args[0].text.bytes > call->reply_capacity)
        return MC_E_INVALID;
    for (uint32_t i = 0; i < call->args[0].text.bytes; ++i) {
        const char c = call->args[0].text.data[i];
        call->reply[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    call->reply_bytes = call->args[0].text.bytes;
    call->reply_kind = MC_REPLY_INSERT;
    return MC_OK;
}

static uint32_t MC_CALL count_visible(mc_ctx* ctx, const mc_menu_target* target) {
    (void)ctx;
    return target->url.bytes ? MC_MENU_SHOW : MC_MENU_HIDE;
}
static mc_status MC_CALL count(mc_ctx* ctx, const mc_menu_target* target) {
    ((state_block*)ctx->state)->counted += target->url.bytes;
    return MC_OK;
}

static mc_status MC_CALL clicked(mc_ctx* ctx, const mc_button_event* event) {
    (void)event;
    ++((state_block*)ctx->state)->clicks;
    return MC_OK;
}

static mc_status MC_CALL start(mc_ctx* ctx) {
    ++((state_block*)ctx->state)->starts;
    return MC_OK;
}

static void MC_CALL stop(mc_ctx* ctx) {
    ++((state_block*)ctx->state)->stops;
}

/* Settings block: prefix (TOGGLE) at 0, style (CHOICE) at 4, note (TEXT) at 8.
   style shows while prefix is on; note while style is "Long". */
static const mc_str choices[] = {MC_S("Short"), MC_S("Long")};
static const mc_setting_def settings[] = {
    {MC_S("prefix"), MC_S("Add a prefix"), {0}, {0}, {0}, 0, 0, MC_SET_TOGGLE, 0, 0, 0, 0, 0, 1, 0, 0, 0},
    {MC_S("style"), MC_S("Style"), {0}, {0}, {0}, choices, 2, MC_SET_CHOICE, 4, 0, 0, 0, 0, 1, 1, 0, 0},
    {MC_S("note"), MC_S("Note"), {0}, {0}, MC_S("long"), 0, 0, MC_SET_TEXT, 8, 0, 0, 0, 0, 0, 2, 0, 1},
};
static const mc_hook hooks[] = {
    {MC_HOOK_PRE_SEND, 0, 0, {.draft = pre_send}},
    {MC_HOOK_DECORATE_MESSAGE, MC_NEED_CONTENT, MC_FILTER_GUILD_ONLY, {.decorate_message = decorate}},
};
static const mc_arg_def shout_args[] = {
    {MC_S("text"), MC_S("What to shout"), MC_ARG_TEXT, MC_ARG_REQUIRED, 0, 0, 0, 0, 0},
};
static const mc_command_def commands[] = {
    {MC_S("shout"), MC_S("Insert text in upper case"), shout_args, 1, 0, shout},
};
static const mc_menu_def menus[] = {
    {MC_S("C tools"), MC_MENU_LINK | MC_MENU_IMAGE, MC_MENU_SUBMENU, 0, 0, 0, 0},
    {MC_S("Count"), MC_MENU_LINK, 0, 1, 0, count_visible, count},
};
static const mc_button_def buttons[] = {
    {MC_SURFACE_CHAT_BAR, MC_BUTTON_TOGGLE, MC_S("Count clicks"), MC_S("lucide/hash"), 0, clicked},
};

/* One row per field group, in declaration order. */
/* clang-format off */
MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    sizeof(mc_descriptor), MC_ABI, 1, 0, 0x6D63635F61626943ull,
    MC_S("C fixture"), MC_S("1.0.0"), MC_S("ABI 6 from plain C."), MC_S("MaiCord"), {0},
    0, 0, 0, sizeof(state_block), 4, 24, 1,
    settings, 3, 2, hooks,
    commands, 1, 2, menus, buttons, 1, 0, 0,
    start, stop};
/* clang-format on */

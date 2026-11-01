/* An ABI-6 plugin for the menu, command and feedback tests (tests/app/plugins/plugin_ui.cpp): context menu
   items with a submenu, a checkbox and visibility rules, typed slash commands with every reply kind, banners,
   modals and the navigation actions. It records what it was asked in its state block, which the test reads. */
#include "maicord/sdk/plugin_abi.h"

#define OWNER 0x4D43525455494658ull

typedef struct state_block {
    uint32_t menu_calls, menu_kind, menu_item, menu_flags, menu_gesture;
    uint64_t user_id, channel_id, guild_id, message_id, item_id;
    uint32_t attachment_index, width, height, url_bytes, text_bytes, type_bytes;
    char url[96], text[48], type[32];
    uint32_t checked, visible_calls;
    uint32_t command_calls, command_gesture, arg_count;
    uint64_t arg_user, arg_channel, command_channel, command_guild;
    int64_t arg_int;
    uint32_t arg_present[8];
    uint32_t arg_bool;
    int32_t banner_status, banner_clear_status, banner_wrong_clear, modal_status, modal_busy_status, url_status,
        copy_status, channel_status, profile_status, bad_status;
    int32_t probe_banner, probe_modal, probe_url, probe_copy, probe_open;
    uint32_t completions, completion_kind, completion_result, completion_toggle, completion_select,
        completion_gesture, completion_text_bytes;
    uint64_t completion_tag;
    char completion_text[64];
    int32_t toast_status, toast_flags_status;
} state_block;

static void copy_str(char* out, uint32_t capacity, uint32_t* bytes, mc_str text) {
    uint32_t n = text.bytes < capacity ? text.bytes : capacity;
    for (uint32_t i = 0; i < n; ++i) out[i] = text.data[i];
    *bytes = n;
}

/* ---- Menus -------------------------------------------------------------------------------------------------- */

static void record_menu(mc_ctx* ctx, const mc_menu_target* t, uint32_t item) {
    state_block* s = (state_block*)ctx->state;
    ++s->menu_calls;
    s->menu_kind = t->kind;
    s->menu_item = item;
    s->menu_flags = t->flags;
    s->menu_gesture = (ctx->flags & MC_CTX_GESTURE) != 0;
    s->user_id = t->user_id;
    s->channel_id = t->channel_id;
    s->guild_id = t->guild_id;
    s->message_id = t->message_id;
    s->item_id = t->item_id;
    s->attachment_index = t->attachment_index;
    s->width = t->width;
    s->height = t->height;
    copy_str(s->url, sizeof(s->url), &s->url_bytes, t->url);
    copy_str(s->text, sizeof(s->text), &s->text_bytes, t->text);
    copy_str(s->type, sizeof(s->type), &s->type_bytes, t->content_type);
}

static mc_status MC_CALL select_inspect(mc_ctx* ctx, const mc_menu_target* t) { record_menu(ctx, t, 0); return MC_OK; }
static mc_status MC_CALL select_link(mc_ctx* ctx, const mc_menu_target* t) { record_menu(ctx, t, 2); return MC_OK; }
static mc_status MC_CALL select_toggle(mc_ctx* ctx, const mc_menu_target* t) {
    state_block* s = (state_block*)ctx->state;
    s->checked = !s->checked;
    record_menu(ctx, t, 3);
    return MC_OK;
}
static mc_status MC_CALL select_danger(mc_ctx* ctx, const mc_menu_target* t) { record_menu(ctx, t, 4); return MC_OK; }
static mc_status MC_CALL select_media(mc_ctx* ctx, const mc_menu_target* t) { record_menu(ctx, t, 7); return MC_OK; }
static mc_status MC_CALL select_fail(mc_ctx* ctx, const mc_menu_target* t) { record_menu(ctx, t, 8); return MC_E_INVALID; }

static uint32_t MC_CALL visible_toggle(mc_ctx* ctx, const mc_menu_target* t) {
    state_block* s = (state_block*)ctx->state;
    (void)t;
    ++s->visible_calls;
    return s->checked ? (MC_MENU_SHOW | MC_MENU_CHECKED) : MC_MENU_SHOW;
}
/* Not shown on your own account. */
static uint32_t MC_CALL visible_others(mc_ctx* ctx, const mc_menu_target* t) {
    state_block* s = (state_block*)ctx->state;
    ++s->visible_calls;
    return (t->flags & MC_MENU_OWN) ? MC_MENU_HIDE : MC_MENU_SHOW;
}
static uint32_t MC_CALL visible_disabled(mc_ctx* ctx, const mc_menu_target* t) {
    (void)ctx;
    (void)t;
    return MC_MENU_DISABLED;
}

static const mc_menu_def menus[] = {
    /* 0 */ {MC_S("Inspect"), MC_MENU_USER | MC_MENU_MESSAGE, 0, 0, 0, 0, select_inspect},
    /* 1 */ {MC_S("More"), MC_MENU_MESSAGE | MC_MENU_LINK, MC_MENU_SUBMENU, 0, 0, 0, 0},
    /* 2 */ {MC_S("Copy link"), MC_MENU_LINK, 0, 2, 0, 0, select_link},
    /* 3 */ {MC_S("Pin to top"), MC_MENU_MESSAGE, MC_MENU_CHECKBOX, 2, 0, visible_toggle, select_toggle},
    /* 4 */ {MC_S("Wipe"), MC_MENU_CHANNEL | MC_MENU_GUILD, MC_MENU_DANGER, 0, 0, 0, select_danger},
    /* 5 */ {MC_S("Others only"), MC_MENU_USER, 0, 0, 0, visible_others, select_inspect},
    /* 6 */ {MC_S("Greyed"), MC_MENU_IMAGE, 0, 0, 0, visible_disabled, select_media},
    /* 7 */ {MC_S("Media"), MC_MENU_EMOJI | MC_MENU_IMAGE, 0, 0, 0, 0, select_media},
    /* 8 */ {MC_S("Broken"), MC_MENU_GUILD, 0, 0, 0, 0, select_fail},
};

/* ---- Commands ------------------------------------------------------------------------------------------------ */

static void begin(mc_ctx* ctx, const mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    ++s->command_calls;
    s->command_gesture = (ctx->flags & MC_CTX_GESTURE) != 0;
    s->arg_count = call->arg_count;
    s->command_channel = call->channel_id;
    s->command_guild = call->guild_id;
    for (uint32_t i = 0; i < call->arg_count && i < 8; ++i) s->arg_present[i] = call->args[i].present;
}

static uint32_t put(mc_command_call* call, uint32_t at, const char* text) {
    for (uint32_t i = 0; text[i] && at < call->reply_capacity; ++i) call->reply[at++] = text[i];
    return at;
}
static uint32_t put_str(mc_command_call* call, uint32_t at, mc_str text) {
    for (uint32_t i = 0; i < text.bytes && at < call->reply_capacity; ++i) call->reply[at++] = text.data[i];
    return at;
}
static uint32_t put_int(mc_command_call* call, uint32_t at, int64_t value) {
    char digits[24];
    uint32_t n = 0;
    uint64_t magnitude = value < 0 ? (uint64_t)(-value) : (uint64_t)value;
    if (value < 0) at = put(call, at, "-");
    do {
        digits[n++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    while (n && at < call->reply_capacity) call->reply[at++] = digits[--n];
    return at;
}
static void finish(mc_command_call* call, uint32_t kind, uint32_t tone, uint32_t bytes) {
    call->reply_kind = kind;
    call->reply_tone = tone;
    call->reply_bytes = bytes;
}

static mc_status MC_CALL run_ping(mc_ctx* ctx, mc_command_call* call) {
    begin(ctx, call);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_say(mc_ctx* ctx, mc_command_call* call) {
    begin(ctx, call);
    finish(call, MC_REPLY_SEND, 0, put_str(call, 0, call->args[0].text));
    return MC_OK;
}
/* note: local message "note:<mode>:<text>" in the warning tone. */
static mc_status MC_CALL run_note(mc_ctx* ctx, mc_command_call* call) {
    uint32_t at = 0;
    begin(ctx, call);
    at = put(call, at, "note:");
    at = call->args[1].present ? put_str(call, at, call->args[1].text) : put(call, at, "-");
    at = put(call, at, ":");
    at = put_str(call, at, call->args[0].text);
    finish(call, MC_REPLY_LOCAL, MC_TONE_WARNING, at);
    return MC_OK;
}
/* calc: insert "a+b=sum". */
static mc_status MC_CALL run_calc(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    int64_t a = call->args[0].integer, b = call->args[1].present ? call->args[1].integer : 0;
    uint32_t at = 0;
    begin(ctx, call);
    s->arg_int = a;
    at = put_int(call, at, a);
    at = put(call, at, "+");
    at = put_int(call, at, b);
    at = put(call, at, "=");
    at = put_int(call, at, a + b);
    finish(call, MC_REPLY_INSERT, 0, at);
    return MC_OK;
}
/* flag: a toast "flag:<on>:<user>:<channel>". */
static mc_status MC_CALL run_flag(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    uint32_t at = 0;
    begin(ctx, call);
    s->arg_bool = call->args[0].present ? (uint32_t)call->args[0].integer : 2;
    s->arg_user = call->args[1].id;
    s->arg_channel = call->args[2].id;
    at = put(call, at, "flag:");
    at = put_int(call, at, (int64_t)s->arg_bool);
    at = put(call, at, ":");
    at = put_int(call, at, (int64_t)s->arg_user);
    at = put(call, at, ":");
    at = put_int(call, at, (int64_t)s->arg_channel);
    finish(call, MC_REPLY_TOAST, MC_TONE_POSITIVE, at);
    return MC_OK;
}
static mc_status MC_CALL run_guild(mc_ctx* ctx, mc_command_call* call) {
    begin(ctx, call);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_fail(mc_ctx* ctx, mc_command_call* call) {
    begin(ctx, call);
    return MC_E_INVALID;
}
/* bad_reply: claims a reply kind the host does not know. */
static mc_status MC_CALL run_bad(mc_ctx* ctx, mc_command_call* call) {
    begin(ctx, call);
    finish(call, 99, 0, 0);
    return MC_OK;
}

static mc_status MC_CALL run_banner(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    mc_banner banner = {MC_TONE_WARNING, 0, 77, MC_S("Plugin banner"), MC_S("Something needs a look"), MC_S("Look")};
    begin(ctx, call);
    s->banner_status = ctx->api->banner(ctx, &banner);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_banner_clear(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    begin(ctx, call);
    s->banner_wrong_clear = ctx->api->banner_clear(ctx, 12345);
    s->banner_clear_status = ctx->api->banner_clear(ctx, 77);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}

static const mc_str form_items[] = {MC_S("one"), MC_S("two"), MC_S("three")};

static mc_status MC_CALL run_confirm(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    mc_modal modal = {0};
    begin(ctx, call);
    modal.flags = MC_MODAL_DANGER;
    modal.title = (mc_str)MC_S("Delete everything?");
    modal.body = (mc_str)MC_S("This cannot be undone.");
    modal.accept = (mc_str)MC_S("Delete");
    modal.toggle_label = (mc_str)MC_S("Also the backups");
    modal.toggle_initial = 1;
    s->modal_status = ctx->api->modal(ctx, &modal, 501);
    s->modal_busy_status = ctx->api->modal(ctx, &modal, 502);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_form(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    mc_modal modal = {0};
    begin(ctx, call);
    modal.title = (mc_str)MC_S("Rename");
    modal.has_input = 1;
    modal.input_label = (mc_str)MC_S("Name");
    modal.input_initial = (mc_str)MC_S("start");
    modal.input_hint = (mc_str)MC_S("a name");
    modal.input_max_chars = 20;
    modal.select_label = (mc_str)MC_S("Kind");
    modal.select_items = form_items;
    modal.select_count = 3;
    modal.select_initial = 1;
    s->modal_status = ctx->api->modal(ctx, &modal, 601);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
/* bad_modal: a toggle together with an input is not offered. */
static mc_status MC_CALL run_bad_modal(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    mc_modal modal = {0};
    begin(ctx, call);
    modal.title = (mc_str)MC_S("Both");
    modal.has_input = 1;
    modal.toggle_label = (mc_str)MC_S("Nope");
    s->bad_status = ctx->api->modal(ctx, &modal, 701);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}

static mc_status MC_CALL run_link(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    begin(ctx, call);
    s->url_status = ctx->api->open_url(ctx, call->args[0].text);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_copy(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    begin(ctx, call);
    s->copy_status = ctx->api->copy_text(ctx, call->args[0].text);
    s->toast_status = ctx->api->toast(ctx, call->args[0].text, MC_TONE_ACCENT, 0);
    s->toast_flags_status = ctx->api->toast(ctx, call->args[0].text, MC_TONE_ACCENT, 1);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}
static mc_status MC_CALL run_goto(mc_ctx* ctx, mc_command_call* call) {
    state_block* s = (state_block*)ctx->state;
    begin(ctx, call);
    s->channel_status = ctx->api->open(ctx, MC_OPEN_CHANNEL, call->args[0].id, 0);
    s->profile_status = ctx->api->open(ctx, MC_OPEN_PROFILE, call->args[1].id, 55);
    s->bad_status = ctx->api->open(ctx, 9, 1, 0);
    finish(call, MC_REPLY_NONE, 0, 0);
    return MC_OK;
}

static const mc_str note_modes[] = {MC_S("plain"), MC_S("loud")};

static const mc_arg_def say_args[] = {
    {MC_S("text"), MC_S("What to say"), MC_ARG_TEXT, MC_ARG_REQUIRED, 0, 0, 0, 0, 0},
};
static const mc_arg_def note_args[] = {
    {MC_S("text"), MC_S("The note"), MC_ARG_TEXT, MC_ARG_REQUIRED, 1, 40, 0, 0, 0},
    {MC_S("mode"), MC_S("How it looks"), MC_ARG_TEXT, 0, 0, 0, note_modes, 2, 0},
};
static const mc_arg_def calc_args[] = {
    {MC_S("a"), MC_S("First number"), MC_ARG_INT, MC_ARG_REQUIRED, -10, 100, 0, 0, 0},
    {MC_S("b"), MC_S("Second number"), MC_ARG_INT, 0, 0, 0, 0, 0, 0},
};
static const mc_arg_def flag_args[] = {
    {MC_S("on"), MC_S("Switch"), MC_ARG_BOOL, 0, 0, 0, 0, 0, 0},
    {MC_S("who"), MC_S("A person"), MC_ARG_USER, 0, 0, 0, 0, 0, 0},
    {MC_S("where"), MC_S("A channel"), MC_ARG_CHANNEL, 0, 0, 0, 0, 0, 0},
};
static const mc_arg_def text_args[] = {
    {MC_S("text"), MC_S("The text"), MC_ARG_TEXT, MC_ARG_REQUIRED, 0, 0, 0, 0, 0},
};
static const mc_arg_def goto_args[] = {
    {MC_S("where"), MC_S("A channel"), MC_ARG_CHANNEL, MC_ARG_REQUIRED, 0, 0, 0, 0, 0},
    {MC_S("who"), MC_S("A person"), MC_ARG_USER, MC_ARG_REQUIRED, 0, 0, 0, 0, 0},
};

static const mc_command_def commands[] = {
    {MC_S("ping"), MC_S("Does nothing"), 0, 0, 0, run_ping},
    {MC_S("say"), MC_S("Sends text"), say_args, 1, 0, run_say},
    {MC_S("note"), MC_S("A note only you see"), note_args, 2, 0, run_note},
    {MC_S("calc"), MC_S("Adds numbers"), calc_args, 2, 0, run_calc},
    {MC_S("flag"), MC_S("Toasts its arguments"), flag_args, 3, 0, run_flag},
    {MC_S("guild-only"), MC_S("Only in servers"), 0, 0, MC_COMMAND_GUILD_ONLY, run_guild},
    {MC_S("fail"), MC_S("Always fails"), 0, 0, 0, run_fail},
    {MC_S("bad-reply"), MC_S("Replies nonsense"), 0, 0, 0, run_bad},
    {MC_S("banner"), MC_S("Shows a banner"), 0, 0, 0, run_banner},
    {MC_S("banner-clear"), MC_S("Removes the banner"), 0, 0, 0, run_banner_clear},
    {MC_S("confirm"), MC_S("Asks to confirm"), 0, 0, 0, run_confirm},
    {MC_S("form"), MC_S("Asks for a name"), 0, 0, 0, run_form},
    {MC_S("bad-modal"), MC_S("Asks for too much"), 0, 0, 0, run_bad_modal},
    {MC_S("link"), MC_S("Opens a link"), text_args, 1, 0, run_link},
    {MC_S("copy"), MC_S("Copies text"), text_args, 1, 0, run_copy},
    {MC_S("goto"), MC_S("Opens a channel and a profile"), goto_args, 2, 0, run_goto},
};

/* ---- Hooks --------------------------------------------------------------------------------------------------- */

/* A cancelled modal's completion carries no gesture: every action that needs one must refuse. */
static void probe(mc_ctx* ctx) {
    state_block* s = (state_block*)ctx->state;
    mc_banner banner = {MC_TONE_NEUTRAL, 0, 5, MC_S("Quiet banner"), {0}, {0}};
    mc_modal modal = {0};
    modal.title = (mc_str)MC_S("Not now");
    s->probe_banner = ctx->api->banner(ctx, &banner);
    s->probe_modal = ctx->api->modal(ctx, &modal, 1);
    s->probe_url = ctx->api->open_url(ctx, (mc_str)MC_S("https://example.com/"));
    s->probe_copy = ctx->api->copy_text(ctx, (mc_str)MC_S("text"));
    s->probe_open = ctx->api->open(ctx, MC_OPEN_CHANNEL, 3, 0);
}

static void MC_CALL completed(mc_ctx* ctx, const mc_completion* c) {
    state_block* s = (state_block*)ctx->state;
    if (c->kind == MC_COMPLETION_MODAL && c->tag == 501 && c->result == 0) probe(ctx);
    ++s->completions;
    s->completion_kind = c->kind;
    s->completion_tag = c->tag;
    s->completion_result = c->result;
    s->completion_toggle = c->toggle;
    s->completion_select = c->select_index;
    s->completion_gesture = (ctx->flags & MC_CTX_GESTURE) != 0;
    copy_str(s->completion_text, sizeof(s->completion_text), &s->completion_text_bytes, c->text);
}

static const mc_hook hooks[] = {
    {MC_HOOK_COMPLETION, 0, 0, {.completion = completed}},
};

MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    .size = sizeof(mc_descriptor),
    .abi = MC_ABI,
    .api_level = 1,
    .owner_id = OWNER,
    .name = MC_S("UI fixture"),
    .version = MC_S("1.0.0"),
    .summary = MC_S("Menus, commands and feedback for the runtime tests."),
    .author = MC_S("MaiCord"),
    .permissions = MC_PERM_MESSAGE_SEND | MC_PERM_CLIPBOARD,
    .state_bytes = sizeof(state_block),
    .state_align = 8,
    .hook_count = 1,
    .hooks = hooks,
    .commands = commands,
    .command_count = sizeof(commands) / sizeof(commands[0]),
    .menu_count = sizeof(menus) / sizeof(menus[0]),
    .menus = menus,
};

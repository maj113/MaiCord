/* ABI-6 plugins for the loader and runtime tests: one source, built once per variant
   (cmake/test_registry.cmake defines exactly one FIXTURE_* macro per target).

     FULL       a valid plugin using settings, the store, toasts, start and stop
     FAILSTART  valid, but start returns an error
     ENTRY      valid, but the DLL has an entry point
     TLS        valid, but the DLL has a TLS directory
     EXTRA      valid, but the DLL exports a function as well as the descriptor
     BAD        the descriptor's name is empty
     LEVEL      the descriptor needs a newer api level than the host has
     SLOW       pre_send sleeps past any small budget
     CRASH      pre_send writes through a null pointer
     NET        HTTP to declared hosts, completions, secret and the other typed settings, setting and keybind hooks */
#include "maicord/sdk/plugin_abi.h"

#if defined(FIXTURE_FULL)
#define OWNER 0x4D43525446554C4Cull
#elif defined(FIXTURE_FAILSTART)
#define OWNER 0x4D43525446535452ull
#elif defined(FIXTURE_ENTRY)
#define OWNER 0x4D43525445545259ull
#elif defined(FIXTURE_TLS)
#define OWNER 0x4D4352545453544Cull
#elif defined(FIXTURE_EXTRA)
#define OWNER 0x4D43525445585452ull
#elif defined(FIXTURE_BAD)
#define OWNER 0x4D43525442414444ull
#elif defined(FIXTURE_LEVEL)
#define OWNER 0x4D4352544C56564Cull
#elif defined(FIXTURE_SLOW)
#define OWNER 0x4D4352545357464Cull
#elif defined(FIXTURE_CRASH)
#define OWNER 0x4D43525443525348ull
#elif defined(FIXTURE_NET)
#define OWNER 0x4D4352544E455421ull
#elif defined(FIXTURE_PIPELINE)
#define OWNER 0x4D43525450495045ull
#elif defined(FIXTURE_PIPELINEAUTO)
#define OWNER 0x4D43525450495041ull
#elif defined(FIXTURE_PIPELINEBARE)
#define OWNER 0x4D43525450495042ull
#else
#error "define one FIXTURE_* variant"
#endif

#if defined(FIXTURE_SLOW)
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
#endif

#if !defined(FIXTURE_NET) && !defined(FIXTURE_PIPELINE) && !defined(FIXTURE_PIPELINEAUTO) && !defined(FIXTURE_PIPELINEBARE)
/* What the full plugin records for the tests to read through ctx->state. */
typedef struct state_block {
    uint32_t starts, stops, sends;
    uint32_t start_flags;
    int32_t toast_status, setting_status, store_set_status, store_get_status, store_missing_status,
        store_truncated_status;
    uint32_t label_before, label_during, store_bytes, count;
} state_block;

typedef struct settings_block {
    uint8_t enabled;
    uint8_t reserved[3];
    int32_t limit;
    mc_str label;
    mc_list words;
    uint32_t mode;
    uint32_t reserved2;
} settings_block;
#endif

#if !defined(FIXTURE_SLOW) && !defined(FIXTURE_CRASH) && !defined(FIXTURE_NET) && !defined(FIXTURE_PIPELINE) && \
    !defined(FIXTURE_PIPELINEAUTO) && !defined(FIXTURE_PIPELINEBARE)
static void put(mc_draft* draft, const char* text, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) draft->buffer[i] = text[i];
    draft->replaced_bytes = bytes;
    draft->verdict = MC_DRAFT_REPLACE;
}
#endif

#if defined(FIXTURE_FULL)

static int equals(mc_str text, const char* word) {
    uint32_t i = 0;
    for (; word[i]; ++i)
        if (i >= text.bytes || text.data[i] != word[i]) return 0;
    return i == text.bytes;
}

static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    state_block* state = (state_block*)ctx->state;
    const settings_block* settings = (const settings_block*)ctx->settings;
    ++state->sends;
    if (equals(draft->text, "cancel")) {
        draft->verdict = MC_DRAFT_CANCEL;
    } else if (equals(draft->text, "replace")) {
        put(draft, "replaced", 8);
    } else if (equals(draft->text, "silent")) {
        draft->flags |= MC_DRAFT_SILENT | MC_DRAFT_REPLY_PING;
    } else if (equals(draft->text, "confirm")) {
        put(draft, "Sure?", 5);
        draft->verdict = MC_DRAFT_CONFIRM;
    } else if (equals(draft->text, "silentfail")) {
        draft->flags |= MC_DRAFT_SILENT;
        return MC_E_INVALID;
    } else if (equals(draft->text, "gesture")) {
        state->count = ctx->flags;
    } else if (equals(draft->text, "invalid")) {
        draft->buffer[0] = (char)0xff;
        draft->replaced_bytes = 1;
        draft->verdict = MC_DRAFT_REPLACE;
    } else if (equals(draft->text, "fail")) {
        put(draft, "ignored", 7);
        return MC_E_INVALID;
    } else if (equals(draft->text, "toast")) {
        const mc_str text = MC_S("hello");
        state->toast_status = ctx->api->toast(ctx, text, MC_TONE_ACCENT, 0);
    } else if (equals(draft->text, "setting")) {
        const char value[] = "new label";
        state->label_before = settings->label.bytes;
        state->setting_status = ctx->api->set_setting(ctx, 2, value, sizeof(value) - 1u);
        state->label_during = ((const settings_block*)ctx->settings)->label.bytes;
    } else if (equals(draft->text, "store")) {
        const mc_str key = MC_S("k");
        const mc_str missing = MC_S("missing");
        char out[8];
        uint32_t bytes = 0;
        state->store_set_status = ctx->api->store_set(ctx, key, "value", 5);
        state->store_get_status = ctx->api->store_get(ctx, key, out, sizeof(out), &bytes);
        state->store_bytes = bytes;
        state->store_missing_status = ctx->api->store_get(ctx, missing, out, sizeof(out), &bytes);
        state->store_truncated_status = ctx->api->store_get(ctx, key, out, 2, &bytes);
    } else if (equals(draft->text, "count")) {
        const mc_str key = MC_S("count");
        uint32_t n = 0, bytes = 0;
        ctx->api->store_get(ctx, key, &n, sizeof(n), &bytes);
        ++n;
        ctx->api->store_set(ctx, key, &n, sizeof(n));
        state->count = n;
    }
    return MC_OK;
}

static mc_status MC_CALL pre_edit(mc_ctx* ctx, mc_draft* draft) {
    const settings_block* settings = (const settings_block*)ctx->settings;
    if (!settings->enabled || draft->text.bytes + 1u > draft->buffer_capacity) return MC_OK;
    for (uint32_t i = 0; i < draft->text.bytes; ++i) draft->buffer[i] = draft->text.data[i];
    draft->buffer[draft->text.bytes] = '!';
    draft->replaced_bytes = draft->text.bytes + 1u;
    draft->verdict = MC_DRAFT_REPLACE;
    return MC_OK;
}

static mc_status MC_CALL start(mc_ctx* ctx) {
    state_block* state = (state_block*)ctx->state;
    ++state->starts;
    state->start_flags = ctx->flags;
    return MC_OK;
}

/* Stop is observable after the plugin is gone: it counts itself in the persisted store. */
static void MC_CALL stop(mc_ctx* ctx) {
    const mc_str key = MC_S("stops");
    uint32_t n = 0, bytes = 0;
    ++((state_block*)ctx->state)->stops;
    ctx->api->store_get(ctx, key, &n, sizeof(n), &bytes);
    ++n;
    ctx->api->store_set(ctx, key, &n, sizeof(n));
}

static const mc_str modes[] = {MC_S("x"), MC_S("y"), MC_S("z")};
static const mc_str words[] = {MC_S("a"), MC_S("b")};
static const mc_setting_def settings[] = {
    {MC_S("enabled"), MC_S("Enabled"), {0}, {0}, {0}, 0, 0, MC_SET_TOGGLE, 0, 0, 0, 0, 0, 1, 0, 0, 0},
    {MC_S("limit"), MC_S("Limit"), {0}, {0}, {0}, 0, 0, MC_SET_INT, 4, 0, 0, 100, 0, 10, 0, 0, 0},
    {MC_S("label"), MC_S("Label"), {0}, {0}, MC_S("hello"), 0, 0, MC_SET_TEXT, 8, 0, 0, 0, 0, 0, 0, 16, 0},
    {MC_S("words"), MC_S("Words"), {0}, {0}, {0}, words, 2, MC_SET_TEXT_LIST, 24, 0, 0, 0, 0, 0, 0, 8, 0},
    {MC_S("mode"), MC_S("Mode"), {0}, {0}, {0}, modes, 3, MC_SET_CHOICE, 40, 0, 0, 0, 0, 2, 0, 0, 0},
};
static const mc_hook hooks[] = {{MC_HOOK_PRE_SEND, 0, 0, {pre_send}}, {MC_HOOK_PRE_EDIT, 0, 0, {pre_edit}}};

#elif defined(FIXTURE_NET)

/* The network plugin. pre_send drives it: "get <url>", "post <url>" (a body and two headers), "small <url>"
   (a response limit of 8 bytes), "url <url>" (remember only). A modal completion tagged 50 starts a request
   for the remembered url, tag 100, from inside a gesture; the completion tagged 100 starts one more (101). */
typedef struct net_state {
    uint32_t sends, completions, setting_changes, keybinds;
    uint32_t last_kind, last_error, last_status, last_body_bytes, last_flags, body_sum;
    uint32_t last_setting, last_keybind, chained_flags, modal_result, modal_toggle, modal_select;
    uint64_t last_tag;
    int32_t submit_status, chained_status, gesture_status;
    uint32_t url_bytes;
    char url[128];
} net_state;

typedef struct net_settings {
    mc_str secret;
    uint32_t colour, key;
    uint64_t channel;
    mc_ids users;
    mc_str label;
} net_settings;

static int starts_with(mc_str text, const char* word) {
    uint32_t i = 0;
    for (; word[i]; ++i)
        if (i >= text.bytes || text.data[i] != word[i]) return 0;
    return 1;
}

static mc_str rest(mc_str text, uint32_t skip) {
    mc_str out = {text.data + skip, text.bytes - skip, 0};
    return out;
}

static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    net_state* state = (net_state*)ctx->state;
    mc_http_request request = {MC_HTTP_GET, 0, {0, 0, 0}, 0, 0, 0, 0, 0, 0};
    static const mc_http_header headers[] = {{MC_S("Content-Type"), MC_S("text/plain")}, {MC_S("X-Key"), MC_S("k")}};
    uint32_t tag = 0;
    ++state->sends;
    if (starts_with(draft->text, "url ")) {
        const mc_str url = rest(draft->text, 4);
        for (uint32_t i = 0; i < url.bytes && i < sizeof(state->url); ++i) state->url[i] = url.data[i];
        state->url_bytes = url.bytes;
        return MC_OK;
    }
    if (starts_with(draft->text, "get ")) {
        request.url = rest(draft->text, 4);
        tag = 1;
    } else if (starts_with(draft->text, "post ")) {
        request.method = MC_HTTP_POST;
        request.url = rest(draft->text, 5);
        request.headers = headers;
        request.header_count = 2;
        request.body = "payload";
        request.body_bytes = 7;
        tag = 2;
    } else if (starts_with(draft->text, "small ")) {
        request.url = rest(draft->text, 6);
        request.max_response_bytes = 8;
        tag = 3;
    } else {
        return MC_OK;
    }
    state->submit_status = ctx->api->http(ctx, &request, tag);
    return MC_OK;
}

static mc_status start_url(mc_ctx* ctx, uint64_t tag, int32_t* status) {
    net_state* state = (net_state*)ctx->state;
    mc_http_request request = {MC_HTTP_GET, 0, {state->url, state->url_bytes, 0}, 0, 0, 0, 0, 0, 0};
    *status = ctx->api->http(ctx, &request, tag);
    return MC_OK;
}

static void MC_CALL completion(mc_ctx* ctx, const mc_completion* c) {
    net_state* state = (net_state*)ctx->state;
    ++state->completions;
    state->last_kind = c->kind;
    state->last_tag = c->tag;
    state->last_flags = ctx->flags;
    if (c->kind == MC_COMPLETION_MODAL) {
        state->modal_result = c->result;
        state->modal_toggle = c->toggle;
        state->modal_select = c->select_index;
        if (c->tag == 50) start_url(ctx, 100, &state->gesture_status);
        return;
    }
    if (c->kind != MC_COMPLETION_HTTP) return;
    state->last_error = c->error;
    state->last_status = c->status;
    state->last_body_bytes = c->body_bytes;
    state->body_sum = 0;
    for (uint32_t i = 0; i < c->body_bytes; ++i) state->body_sum += ((const unsigned char*)c->body)[i];
    if (c->tag == 100) start_url(ctx, 101, &state->chained_status);
    if (c->tag == 101) state->chained_flags = ctx->flags;
}

static void MC_CALL setting_changed(mc_ctx* ctx, uint32_t index) {
    net_state* state = (net_state*)ctx->state;
    ++state->setting_changes;
    state->last_setting = index;
    state->last_flags = ctx->flags;
}

static void MC_CALL keybind(mc_ctx* ctx, uint32_t index) {
    net_state* state = (net_state*)ctx->state;
    ++state->keybinds;
    state->last_keybind = index;
    state->last_flags = ctx->flags;
}

static const mc_str network[] = {MC_S("api.example.com"), MC_S("*.cdn.example.net"), MC_S("other.example.org")};
static const mc_setting_def settings[] = {
    {MC_S("secret"), MC_S("API key"), {0}, {0}, {0}, 0, 0, MC_SET_SECRET, 0, 0, 0, 0, 0, 0, 0, 64, 0},
    {MC_S("colour"), MC_S("Colour"), {0}, {0}, {0}, 0, 0, MC_SET_COLOUR, 16, 0, 0, 0, 0, 0x336699, 0, 0, 0},
    {MC_S("key"), MC_S("Shortcut"), {0}, {0}, {0}, 0, 0, MC_SET_KEYBIND, 20, 0, 0, 0, 0, 0, 0, 0, 0},
    {MC_S("channel"), MC_S("Channel"), {0}, {0}, {0}, 0, 0, MC_SET_CHANNEL, 24, 0, 0, 0, 0, 0, 0, 0, 0},
    {MC_S("users"), MC_S("People"), {0}, {0}, {0}, 0, 0, MC_SET_USER_LIST, 32, 0, 0, 0, 0, 0, 0, 0, 0},
    {MC_S("label"), MC_S("Label"), {0}, {0}, MC_S("hi"), 0, 0, MC_SET_TEXT, 48, 0, 0, 0, 0, 0, 0, 16, 0},
};
static const mc_hook hooks[] = {{MC_HOOK_PRE_SEND, 0, 0, {pre_send}},
                                {MC_HOOK_SETTING_CHANGED, 0, 0, {.setting = setting_changed}},
                                {MC_HOOK_KEYBIND, 0, 0, {.setting = keybind}},
                                {MC_HOOK_COMPLETION, 0, 0, {.completion = completion}}};

#elif defined(FIXTURE_PIPELINE) || defined(FIXTURE_PIPELINEAUTO) || defined(FIXTURE_PIPELINEBARE)

/* The message pipeline plugin. Hooks record what they are given and answer by the message text:
   a message starting "keep" is kept (deleted and edited), "handle" handles a click, channel 0x5555 has its typing
   suppressed. pre_send drives the actions by its text ("send", "edit", "react", "delete", "read", "activity",
   "local", "composer", "timer", "restore", "keep"); the statuses land in state.status. A timer sends. */
typedef struct state_block {
    uint32_t clicks, deleted, edited, typing, timers, batches, events, overflows;
    uint32_t click_flags, click_button, click_count, click_mods;
    uint32_t deleted_flags, deleted_bytes, edited_before, edited_after, last_event_flags, last_event_text;
    uint32_t kinds_seen, largest_batch, kept_flags, kept_edits, kept_text_bytes;
    uint64_t overflow_value, last_timer, local_id;
    int32_t status[16];
    int32_t click_react_status, timer_send_status, truncated_status;
} state_block;

static int begins(mc_str text, const char* word) {
    uint32_t i = 0;
    for (; word[i]; ++i)
        if (i >= text.bytes || text.data[i] != word[i]) return 0;
    return 1;
}

static uint32_t MC_CALL click(mc_ctx* ctx, const mc_message* message, const mc_click* click) {
    state_block* state = (state_block*)ctx->state;
    const mc_str emoji = MC_S("ok");
    ++state->clicks;
    state->click_flags = ctx->flags;
    state->click_button = click->button;
    state->click_count = click->count;
    state->click_mods = click->modifiers;
    if (begins(message->content, "react"))
        state->click_react_status = ctx->api->react(ctx, message->channel_id, message->message_id, emoji, 0);
    return begins(message->content, "handle") ? MC_CLICK_HANDLED : MC_CLICK_PASS;
}

static uint32_t MC_CALL deleted(mc_ctx* ctx, const mc_message* message) {
    state_block* state = (state_block*)ctx->state;
    ++state->deleted;
    state->deleted_flags = message->flags;
    state->deleted_bytes = message->content.bytes;
    return begins(message->content, "keep") ? MC_KEEP : MC_DROP;
}

static uint32_t MC_CALL edited(mc_ctx* ctx, const mc_message* before, const mc_message* after) {
    state_block* state = (state_block*)ctx->state;
    ++state->edited;
    state->edited_before = before->content.bytes;
    state->edited_after = after->content.bytes;
    return begins(after->content, "keep") ? MC_KEEP : MC_DROP;
}

static void MC_CALL events(mc_ctx* ctx, const mc_event* list, uint32_t count) {
    state_block* state = (state_block*)ctx->state;
    ++state->batches;
    if (count > state->largest_batch) state->largest_batch = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (list[i].kind == MC_EVENT_OVERFLOW) {
            ++state->overflows;
            state->overflow_value = list[i].value;
            continue;
        }
        ++state->events;
        state->kinds_seen |= 1u << list[i].kind;
        state->last_event_flags = list[i].flags;
        state->last_event_text = list[i].text.bytes;
    }
}

static uint32_t MC_CALL typing(mc_ctx* ctx, uint64_t channel) {
    ++((state_block*)ctx->state)->typing;
    return channel == 0x5555 ? MC_SUPPRESS : MC_ALLOW;
}

static void MC_CALL timer(mc_ctx* ctx, uint64_t id) {
    state_block* state = (state_block*)ctx->state;
    const mc_send send = {9, 0, MC_S("tick"), 0, 0};
    ++state->timers;
    state->last_timer = id;
    state->timer_send_status = ctx->api->send_message(ctx, &send);
}

static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    state_block* state = (state_block*)ctx->state;
    const mc_api* api = ctx->api;
    const mc_send send = {9, 0, MC_S("hi"), 0, 0};
    const mc_str edit = MC_S("e"), emoji = MC_S("ok"), note = MC_S("note"), a = MC_S("a"), b = MC_S("b");
    const mc_activity activity = {MC_ACTIVITY_PLAYING, 0, 0, 0, 0, MC_S("game"), {0}, {0}, {0}, {0}};
    mc_kept_info info;
    char out[16];
    uint32_t bytes = 0;
    if (begins(draft->text, "send")) {
        state->status[0] = api->send_message(ctx, &send);
    } else if (begins(draft->text, "edit")) {
        state->status[1] = api->edit_message(ctx, 9, 10, edit);
    } else if (begins(draft->text, "react")) {
        state->status[2] = api->react(ctx, 9, 10, emoji, 0);
    } else if (begins(draft->text, "delete")) {
        state->status[3] = api->delete_own(ctx, 9, 10);
    } else if (begins(draft->text, "read")) {
        state->status[4] = api->mark_read(ctx, MC_READ_CHANNEL, 9);
    } else if (begins(draft->text, "activity")) {
        state->status[5] = api->set_activity(ctx, &activity);
        state->status[6] = api->set_activity(ctx, 0);
    } else if (begins(draft->text, "local")) {
        state->status[7] = api->local_message(ctx, 9, note, &state->local_id);
    } else if (begins(draft->text, "composer")) {
        state->status[8] = api->composer_insert(ctx, a);
        state->status[9] = api->composer_replace(ctx, b);
        state->status[10] = api->composer_focus(ctx);
        state->status[11] = api->composer_text(ctx, out, 1, &bytes);
    } else if (begins(draft->text, "timer")) {
        state->status[12] = api->timer(ctx, 1, 100, 0);
        state->status[13] = api->timer(ctx, 2, 50, 0);
    } else if (begins(draft->text, "restore")) {
        const mc_str edits[] = {MC_S("older"), MC_S("oldest")};
        const mc_kept item = {MC_KEPT_DELETED, 2, 9, 77, 5, 1000, 0, MC_S("gone"), MC_S("Ann"), edits};
        state->status[14] = api->keep_restore(ctx, &item, 1);
    } else if (begins(draft->text, "keep")) {
        state->status[15] = api->keep_info(ctx, 9, 77, &info);
        if (state->status[15] == MC_OK) {
            state->kept_flags = info.flags;
            state->kept_edits = info.edit_count;
        }
        state->truncated_status = api->keep_text(ctx, 9, 77, 1, out, 2, &bytes);
        state->kept_text_bytes = bytes;
    }
    return MC_OK;
}

static const mc_hook hooks[] = {{MC_HOOK_PRE_SEND, 0, 0, {pre_send}},
                                {MC_HOOK_MESSAGE_CLICK, 0, 0, {.click = click}},
                                {MC_HOOK_MESSAGE_DELETED, 0, 0, {.deleted = deleted}},
                                {MC_HOOK_MESSAGE_EDITED, 0, 0, {.edited = edited}},
                                {MC_HOOK_EVENTS, 0, MC_EVENT_KINDS, {.events = events}},
                                {MC_HOOK_PRE_TYPING, 0, 0, {.typing = typing}},
                                {MC_HOOK_TIMER, 0, 0, {.timer = timer}}};

#else

#if defined(FIXTURE_SLOW)
static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    (void)ctx;
    (void)draft;
    Sleep(20);
    return MC_OK;
}
#elif defined(FIXTURE_CRASH)
static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    (void)ctx;
    (void)draft;
    *(volatile int*)0 = 1;
    return MC_OK;
}
#else
static mc_status MC_CALL pre_send(mc_ctx* ctx, mc_draft* draft) {
    (void)ctx;
    put(draft, "x", 1);
    return MC_OK;
}
#endif
#if defined(FIXTURE_FAILSTART)
static mc_status MC_CALL start(mc_ctx* ctx) {
    (void)ctx;
    return MC_E_INVALID;
}
#endif
static const mc_hook hooks[] = {{MC_HOOK_PRE_SEND, 0, 0, {pre_send}}};

#endif

#if defined(FIXTURE_ENTRY)
int __stdcall fixture_entry(void* instance, unsigned long reason, void* reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
#endif

#if defined(FIXTURE_TLS)
/* What the CRT's tlssup provides: a directory with no callbacks, found through _tls_used. */
static unsigned char tls_template[8];
static unsigned long tls_index;
#pragma section(".rdata$T", read)
__declspec(allocate(".rdata$T")) const struct {
    unsigned long long start, end, index, callbacks;
    unsigned long zero, characteristics;
} _tls_used = {(unsigned long long)tls_template, (unsigned long long)(tls_template + sizeof(tls_template)),
               (unsigned long long)&tls_index, 0, 0, 0};
#endif

#if defined(FIXTURE_EXTRA)
MC_EXPORT int maicord_fixture_extra(void) { return 7; }
#endif

#if defined(FIXTURE_BAD)
#define NAME {"", 0, 0}
#else
#define NAME MC_S("Runtime fixture")
#endif

#if defined(FIXTURE_LEVEL)
#define LEVEL 2
#else
#define LEVEL 1
#endif

#if defined(FIXTURE_NET)
#define STATE_BYTES sizeof(net_state)
#define SETTINGS_BYTES sizeof(net_settings)
#define SETTING_COUNT 6
#define SETTING_TABLE settings
#define HOOK_COUNT 4
#define NETWORK_TABLE network
#define NETWORK_COUNT 3
#define START 0
#define STOP 0
#elif defined(FIXTURE_PIPELINE) || defined(FIXTURE_PIPELINEAUTO) || defined(FIXTURE_PIPELINEBARE)
#define STATE_BYTES sizeof(state_block)
#define SETTINGS_BYTES 0
#define SETTING_COUNT 0
#define SETTING_TABLE 0
#define HOOK_COUNT 7
#define START 0
#define STOP 0
#if defined(FIXTURE_PIPELINEAUTO)
#define PERMISSIONS (MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_COMPOSER | MC_PERM_PERSISTENT_LOG | MC_PERM_AUTOMATED_ACTIONS)
#elif defined(FIXTURE_PIPELINE)
#define PERMISSIONS (MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_COMPOSER | MC_PERM_PERSISTENT_LOG)
#endif
#elif defined(FIXTURE_FULL)
#define STATE_BYTES sizeof(state_block)
#define SETTINGS_BYTES sizeof(settings_block)
#define SETTING_COUNT 5
#define SETTING_TABLE settings
#define HOOK_COUNT 2
#define START start
#define STOP stop
#elif defined(FIXTURE_FAILSTART)
#define STATE_BYTES 0
#define SETTINGS_BYTES 0
#define SETTING_COUNT 0
#define SETTING_TABLE 0
#define HOOK_COUNT 1
#define START start
#define STOP 0
#else
#define STATE_BYTES 0
#define SETTINGS_BYTES 0
#define SETTING_COUNT 0
#define SETTING_TABLE 0
#define HOOK_COUNT 1
#define START 0
#define STOP 0
#endif

#ifndef NETWORK_TABLE
#define NETWORK_TABLE 0
#define NETWORK_COUNT 0
#endif

#ifndef PERMISSIONS
#define PERMISSIONS 0
#endif

MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    sizeof(mc_descriptor), MC_ABI, LEVEL, 0, OWNER,
    NAME, MC_S("1.0.0"), MC_S("ABI 6 loader and runtime fixture."), MC_S("MaiCord"), {0},
    PERMISSIONS, NETWORK_TABLE, NETWORK_COUNT, STATE_BYTES, STATE_BYTES ? 4 : 0, SETTINGS_BYTES, 1,
    SETTING_TABLE, SETTING_COUNT, HOOK_COUNT, hooks,
    0, 0, 0, 0, 0, 0, 0, 0,
    START, STOP};

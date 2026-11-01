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
     CRASH      pre_send writes through a null pointer */
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
#else
#error "define one FIXTURE_* variant"
#endif

#if defined(FIXTURE_SLOW)
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
#endif

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

#if !defined(FIXTURE_SLOW) && !defined(FIXTURE_CRASH)
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

#if defined(FIXTURE_FULL)
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

MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    sizeof(mc_descriptor), MC_ABI, LEVEL, 0, OWNER,
    NAME, MC_S("1.0.0"), MC_S("ABI 6 loader and runtime fixture."), MC_S("MaiCord"), {0},
    0, 0, 0, STATE_BYTES, STATE_BYTES ? 4 : 0, SETTINGS_BYTES, 1,
    SETTING_TABLE, SETTING_COUNT, HOOK_COUNT, hooks,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    START, STOP};

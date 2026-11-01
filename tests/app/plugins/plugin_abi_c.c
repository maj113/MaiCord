/* A plain C11 plugin against maicord/sdk/plugin_abi.h: the C path of ABI 6
   and the frozen layouts. Its pre_send prefixes "[c] " and cancels "cancel";
   start and stop count in the state block. */
#include "maicord/sdk/plugin_abi.h"

_Static_assert(sizeof(mc_str) == 16 && offsetof(mc_str, bytes) == 8, "mc_str");
_Static_assert(sizeof(mc_list) == 16 && offsetof(mc_list, count) == 8, "mc_list");
_Static_assert(sizeof(mc_ctx) == 32 && offsetof(mc_ctx, settings) == 16 && offsetof(mc_ctx, flags) == 24 &&
                   offsetof(mc_ctx, call) == 28,
               "mc_ctx");
_Static_assert(sizeof(mc_setting_def) == 128 && offsetof(mc_setting_def, default_text) == 64 &&
                   offsetof(mc_setting_def, items) == 80 && offsetof(mc_setting_def, item_count) == 88 &&
                   offsetof(mc_setting_def, type) == 92 && offsetof(mc_setting_def, offset) == 96 &&
                   offsetof(mc_setting_def, minimum) == 104 &&
                   offsetof(mc_setting_def, default_int) == 116 &&
                   offsetof(mc_setting_def, visible_if) == 120 &&
                   offsetof(mc_setting_def, max_bytes) == 122,
               "mc_setting_def");
_Static_assert(sizeof(mc_hook) == 24 && offsetof(mc_hook, filter) == 8 && offsetof(mc_hook, fn) == 16,
               "mc_hook");
_Static_assert(sizeof(mc_draft) == 72 && offsetof(mc_draft, text) == 32 &&
                   offsetof(mc_draft, buffer) == 48 && offsetof(mc_draft, buffer_capacity) == 56 &&
                   offsetof(mc_draft, replaced_bytes) == 60 && offsetof(mc_draft, flags) == 64 &&
                   offsetof(mc_draft, verdict) == 68,
               "mc_draft");
_Static_assert(sizeof(mc_descriptor) == 248 && offsetof(mc_descriptor, owner_id) == 16 &&
                   offsetof(mc_descriptor, name) == 24 && offsetof(mc_descriptor, permissions) == 104 &&
                   offsetof(mc_descriptor, network) == 112 && offsetof(mc_descriptor, state_bytes) == 124 &&
                   offsetof(mc_descriptor, settings_bytes) == 132 &&
                   offsetof(mc_descriptor, state_layout) == 136 &&
                   offsetof(mc_descriptor, settings) == 144 && offsetof(mc_descriptor, hook_count) == 156 &&
                   offsetof(mc_descriptor, hooks) == 160 && offsetof(mc_descriptor, commands) == 168 &&
                   offsetof(mc_descriptor, menus) == 184 && offsetof(mc_descriptor, buttons) == 192 &&
                   offsetof(mc_descriptor, theme) == 208 && offsetof(mc_descriptor, effects) == 216 &&
                   offsetof(mc_descriptor, start) == 232 && offsetof(mc_descriptor, stop) == 240,
               "mc_descriptor");
_Static_assert(offsetof(mc_api, log) == 8 && offsetof(mc_api, set_setting) == 16 &&
                   offsetof(mc_api, store_get) == 24 && offsetof(mc_api, store_set) == 32 &&
                   offsetof(mc_api, store_remove) == 40 && offsetof(mc_api, toast) == 48 &&
                   sizeof(mc_api) == 56,
               "mc_api level 1");

typedef struct state_block {
    uint32_t starts;
    uint32_t stops;
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

static mc_status MC_CALL start(mc_ctx* ctx) {
    ++((state_block*)ctx->state)->starts;
    return MC_OK;
}

static void MC_CALL stop(mc_ctx* ctx) {
    ++((state_block*)ctx->state)->stops;
}

static const mc_str choices[] = {MC_S("Short"), MC_S("Long")};
static const mc_setting_def settings[] = {
    {MC_S("prefix"), MC_S("Add a prefix"), {0}, {0}, {0}, 0, 0, MC_SET_TOGGLE, 0, 0, 0, 0, 0, 1, 0, 0, 0},
    {MC_S("style"), MC_S("Style"), {0}, {0}, {0}, choices, 2, MC_SET_CHOICE, 4, 0, 0, 0, 0, 1, 0, 0, 0},
};
static const mc_hook hooks[] = {{MC_HOOK_PRE_SEND, 0, 0, {pre_send}}};

/* One row per field group, in declaration order. */
/* clang-format off */
MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    sizeof(mc_descriptor), MC_ABI, 1, 0, 0x6D63635F61626943ull,
    MC_S("C fixture"), MC_S("1.0.0"), MC_S("ABI 6 from plain C."), MC_S("MaiCord"), {0},
    0, 0, 0, sizeof(state_block), 4, 8, 1,
    settings, 2, 1, hooks,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    start, stop};
/* clang-format on */

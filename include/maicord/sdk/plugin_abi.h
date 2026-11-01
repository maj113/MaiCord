/*
 * MaiCord native plugin SDK, ABI 6. Pure C (C11 or C++), Windows x64 only.
 *
 * OVERVIEW
 *   A plugin is a DLL whose only export is one const data object:
 *
 *     MC_EXPORT const mc_descriptor maicord_plugin_v6 = { ... };
 *
 *   The DLL has no entry point and no TLS callbacks (link /NOENTRY, no CRT),
 *   so loading it runs none of its code. The host checks this in the PE
 *   headers before LoadLibrary, then validates the descriptor once: strings,
 *   counts, settings offsets, hook kinds and api_level. Everything the
 *   plugin does is declared there: metadata, permissions, settings schema,
 *   state size and hooks. maicord/sdk/plugin.hpp generates the descriptor
 *   from C++ declarations; C plugins write it by hand.
 *
 * CALLS
 *   Every entry point (start, stop, hooks) receives an mc_ctx* that is valid
 *   until it returns. Host functions are reached through ctx->api; a stale
 *   context makes them fail with MC_E_INACTIVE. Every call happens on the UI
 *   thread. Strings are UTF-8, not NUL-terminated, and valid for the call;
 *   copy what you keep. State must not hold pointers into the DLL image or
 *   into memory the host passed in.
 *
 * VERSIONING
 *   The host supports exactly one ABI. Within ABI 6 the mc_api table and
 *   the structs the host writes only grow by appending; each addition bumps
 *   MC_API_LEVEL. A plugin declares the api_level it needs and a host with a
 *   lower level refuses to load it, so a plugin may use every field and
 *   function of its level. MC_API_HAS tests for newer optional entries. The
 *   descriptor and the structs it points to are frozen; new capabilities
 *   append descriptor fields, read up to descriptor.size.
 *
 * SAFETY
 *   Every call into plugin code is timed against a budget per call class
 *   (hooks 10 ms; start and stop 500 ms). Repeated overruns turn the plugin
 *   off. The host records which plugin call is in progress, so a crash or
 *   hang is attributed and the plugin stays off at the next start. This is
 *   attribution, not a sandbox: plugins are trusted native code. No host
 *   function exposes tokens, credentials, cookies or raw network access.
 */
#ifndef MAICORD_PLUGIN_ABI_H
#define MAICORD_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_ABI 6u
#define MC_API_LEVEL 1u /* highest mc_api level this header describes */
#define MC_CALL __stdcall
#define MC_EXPORT __declspec(dllexport)
#define MC_DESCRIPTOR_SYMBOL maicord_plugin_v6
#define MC_DESCRIPTOR_EXPORT "maicord_plugin_v6"

typedef struct mc_str {
    const char* data; /* UTF-8, not NUL-terminated */
    uint32_t bytes;
    uint32_t reserved; /* 0 */
} mc_str;
/* An mc_str initializer from a string literal. */
#define MC_S(literal) {(literal), (uint32_t)(sizeof(literal) - 1u), 0u}

typedef struct mc_list {
    const mc_str* items;
    uint32_t count;
    uint32_t reserved; /* 0 */
} mc_list;

typedef int32_t mc_status;
enum {
    MC_OK = 0,
    MC_E_INVALID = 1,      /* malformed argument */
    MC_E_DENIED = 2,       /* permission not declared or not approved */
    MC_E_BUDGET = 3,       /* per-plugin quota reached */
    MC_E_NOT_FOUND = 4,    /* unknown key or id */
    MC_E_INACTIVE = 5,     /* the context is stale, or the call isn't allowed now */
    MC_E_UNSUPPORTED = 6,  /* not available at this host's level */
    MC_E_CAPACITY = 7,     /* value larger than the host accepts */
    MC_E_TRUNCATED = 8,    /* output did not fit; the needed size is reported */
    MC_E_BUSY = 9,         /* the host can't take this now */
    MC_E_RATE_LIMITED = 10 /* the rate limit is exhausted; retry later */
};

/* Permissions the user approves at enable. Hooks imply theirs; the host ORs
   them into descriptor.permissions, so plugins list only API permissions. */
#define MC_PERM_MESSAGE_SEND (1ull << 0) /* change your messages before they're sent or edited */
#define MC_PERM_KNOWN (MC_PERM_MESSAGE_SEND)

/* Tones map to theme roles; plugins never choose colours. */
enum {
    MC_TONE_NEUTRAL = 0,
    MC_TONE_ACCENT = 1,
    MC_TONE_POSITIVE = 2,
    MC_TONE_WARNING = 3,
    MC_TONE_CRITICAL = 4
};

enum {
    MC_LOG_DEBUG = 0,
    MC_LOG_INFO = 1,
    MC_LOG_WARNING = 2,
    MC_LOG_ERROR = 3
};

/* ---- Context -------------------------------------------------------------- */

typedef struct mc_api mc_api;

#define MC_CTX_GESTURE 1u     /* the user triggered this call directly */
#define MC_CTX_RELOADED 2u    /* start: state survived a reload */
#define MC_CTX_FIRST_START 4u /* start: state was just zeroed */

/* Passed to every plugin entry point. Host-owned; valid until it returns. */
typedef struct mc_ctx {
    const mc_api* api;    /* immutable host table; api->size bounds availability */
    void* state;          /* descriptor.state_bytes, zeroed at first start; null when 0 */
    const void* settings; /* settings block (descriptor.settings_bytes); read-only; null when 0 */
    uint32_t flags;       /* MC_CTX_* */
    uint32_t call;        /* host call token */
} mc_ctx;

/* ---- Settings schema ------------------------------------------------------ */
/* The host owns a block of descriptor.settings_bytes (8-byte aligned) and
   writes each setting's value at its offset in the encoding of its type.
   Values change only between calls. */
enum {
    MC_SET_TOGGLE = 1,     /* uint8_t 0 or 1; default_int */
    MC_SET_INT = 2,        /* int32_t in [minimum, maximum]; default_int */
    MC_SET_SLIDER = 3,     /* int32_t in [minimum, maximum] by step; default_int */
    MC_SET_CHOICE = 4,     /* uint32_t index into items; default_int */
    MC_SET_TEXT = 5,       /* mc_str, at most max_bytes; default_text */
    MC_SET_SECRET = 6,     /* reserved: refused below its api level */
    MC_SET_COLOUR = 7,     /* reserved */
    MC_SET_KEYBIND = 8,    /* reserved */
    MC_SET_TEXT_LIST = 9,  /* mc_list of mc_str, each at most max_bytes; defaults in items */
    MC_SET_CHANNEL = 10,   /* reserved */
    MC_SET_USER_LIST = 11, /* reserved */
    MC_SET_HEADER = 12     /* a section label; no value, offset ignored */
};
#define MC_SET_HIDDEN 1u    /* not shown in the settings page */
#define MC_SET_DANGER 2u    /* critical tone */
#define MC_SET_MULTILINE 4u /* TEXT: multi-line input */

#define MC_SETTING_MAX 64u          /* settings per plugin */
#define MC_SETTINGS_BYTES_MAX 4096u /* descriptor.settings_bytes */
#define MC_SETTING_KEY_MAX 32u      /* key: 1..32 bytes of [a-z0-9_] */
#define MC_SETTING_LABEL_MAX 64u
#define MC_SETTING_TEXT_MAX 1024u       /* description, default_text, values; max_bytes may lower it */
#define MC_SETTING_ITEM_MAX 64u         /* choice labels, list items */
#define MC_SETTING_LIST_BYTES_MAX 4096u /* all items of a TEXT_LIST value */

typedef struct mc_setting_def {
    mc_str key; /* unique; names the persisted value */
    mc_str label;
    mc_str description;  /* optional help text */
    mc_str hint;         /* optional placeholder for text inputs */
    mc_str default_text; /* TEXT */
    const mc_str* items; /* CHOICE: labels; TEXT_LIST: default items */
    uint32_t item_count;
    uint32_t type;   /* MC_SET_* */
    uint32_t offset; /* into the settings block, aligned for the type */
    uint32_t flags;  /* MC_SET_HIDDEN, MC_SET_DANGER, MC_SET_MULTILINE */
    int32_t minimum, maximum, step, default_int;
    uint16_t visible_if; /* 1-based index of a toggle that must be on; 0 = always */
    uint16_t max_bytes;  /* TEXT, TEXT_LIST items; 0 = MC_SETTING_TEXT_MAX */
    uint32_t reserved;   /* 0 */
} mc_setting_def;

/* ---- Hooks ---------------------------------------------------------------- */

enum {
    MC_HOOK_PRE_SEND = 1, /* fn.draft: before a message you typed is sent */
    MC_HOOK_PRE_EDIT = 2, /* fn.draft: before an edit of your message is saved */
    MC_HOOK_LAST = MC_HOOK_PRE_EDIT
};

/* mc_draft.flags (PRE_SEND); in and out. */
#define MC_DRAFT_SILENT 1u /* @silent: no push notification */
#define MC_DRAFT_SUPPRESS_EMBEDS 2u
#define MC_DRAFT_REPLY_PING 4u /* the reply pings the replied-to author */

/* mc_draft.verdict, set by the plugin; the host sets MC_DRAFT_KEEP first. */
enum {
    MC_DRAFT_KEEP = 0,    /* unchanged */
    MC_DRAFT_REPLACE = 1, /* buffer[0, replaced_bytes) replaces text */
    MC_DRAFT_CANCEL = 2   /* don't send or save; the composer keeps the text */
};

#define MC_DRAFT_TEXT_MAX 4000u /* buffer_capacity is at least this */

/* Hooks run in descriptor order per plugin and plugin enable order across
   plugins; each sees the text earlier hooks produced. A status other than
   MC_OK, or an overrun, leaves the draft as it was before the call. */
typedef struct mc_draft {
    uint64_t channel_id;
    uint64_t guild_id;        /* 0 in DMs */
    uint64_t message_id;      /* PRE_EDIT: the message being edited; PRE_SEND: 0 */
    uint64_t reply_to;        /* PRE_SEND: the replied-to message, or 0 */
    mc_str text;              /* in: the current text */
    char* buffer;             /* out: host-owned; never aliases text */
    uint32_t buffer_capacity; /* bytes */
    uint32_t replaced_bytes;  /* out, with MC_DRAFT_REPLACE: valid UTF-8, not empty */
    uint32_t flags;           /* in/out: MC_DRAFT_* (PRE_SEND); PRE_EDIT ignores changes */
    uint32_t verdict;         /* out: MC_DRAFT_KEEP, MC_DRAFT_REPLACE or MC_DRAFT_CANCEL */
} mc_draft;

typedef mc_status(MC_CALL* mc_draft_fn)(mc_ctx* ctx, mc_draft* draft);

/* The member matching kind is called. */
typedef union mc_hook_fn {
    mc_draft_fn draft; /* MC_HOOK_PRE_SEND, MC_HOOK_PRE_EDIT */
} mc_hook_fn;

typedef struct mc_hook {
    uint32_t kind;   /* MC_HOOK_*; at most one hook per kind */
    uint32_t flags;  /* per kind; 0 for PRE_SEND and PRE_EDIT */
    uint64_t filter; /* per kind; 0 for PRE_SEND and PRE_EDIT */
    mc_hook_fn fn;
} mc_hook;

/* ---- Descriptor ----------------------------------------------------------- */

/* Defined at later api levels; their counts must be 0 until then. */
struct mc_command_def;
struct mc_menu_def;
struct mc_button_def;
struct mc_theme_token;
struct mc_fx_def;

#define MC_NAME_MAX 64u     /* name: 1..64 bytes; author: 0..64 */
#define MC_VERSION_MAX 32u  /* version: 1..32 bytes */
#define MC_SUMMARY_MAX 256u /* summary, homepage: 0..256 bytes */
#define MC_STATE_MAX (1u << 20)
#define MC_STATE_ALIGN_MAX 64u

typedef struct mc_descriptor {
    uint32_t size;      /* sizeof(mc_descriptor) as compiled */
    uint32_t abi;       /* MC_ABI */
    uint32_t api_level; /* the mc_api level the plugin needs, 1..MC_API_LEVEL of its header */
    uint32_t flags;     /* 0 */
    uint64_t owner_id;  /* non-zero; identifies the plugin and its data */
    mc_str name, version, summary, author, homepage; /* homepage: empty or https:// */
    uint64_t permissions;                            /* MC_PERM_* the APIs need; hooks add their own */
    const mc_str* network;                           /* reserved: network_count must be 0 */
    uint32_t network_count;
    uint32_t state_bytes;    /* 0..MC_STATE_MAX */
    uint32_t state_align;    /* power of two, 1..MC_STATE_ALIGN_MAX; 0 when state_bytes is 0 */
    uint32_t settings_bytes; /* 0..MC_SETTINGS_BYTES_MAX */
    uint64_t state_layout;   /* equal across a reload keeps state */
    const mc_setting_def* settings;
    uint32_t setting_count; /* 0..MC_SETTING_MAX */
    uint32_t hook_count;    /* 0..MC_HOOK_LAST */
    const mc_hook* hooks;
    const struct mc_command_def* commands;
    uint32_t command_count;
    uint32_t menu_count;
    const struct mc_menu_def* menus;
    const struct mc_button_def* buttons;
    uint32_t button_count;
    uint32_t theme_count;
    const struct mc_theme_token* theme;
    const struct mc_fx_def* effects;
    uint32_t effect_count;
    uint32_t reserved;                      /* 0 */
    mc_status(MC_CALL* start)(mc_ctx* ctx); /* optional; a failure disables the plugin */
    void(MC_CALL* stop)(mc_ctx* ctx);       /* optional */
} mc_descriptor;

/* ---- Host functions ------------------------------------------------------- */

#define MC_LOG_TEXT_MAX 512u
#define MC_TOAST_TEXT_MAX 256u
#define MC_STORE_KEY_MAX 64u           /* keys: 1..64 bytes of printable ASCII (0x21..0x7E) */
#define MC_STORE_VALUE_MAX (64u << 10) /* bytes per value */
#define MC_STORE_KEYS_MAX 256u
#define MC_STORE_TOTAL_MAX (1u << 20) /* bytes of keys and values */

/* Immutable; append-only within ABI 6. level 1 is everything below. */
struct mc_api {
    uint32_t size; /* bytes of this table the host provides */
    uint32_t level;
    /* Appends a line to the plugin's log (dev console, OutputDebugString);
       longer text is cut at MC_LOG_TEXT_MAX. */
    void(MC_CALL* log)(mc_ctx* ctx, uint32_t level, mc_str text);
    /* Changes one of the plugin's own settings (index into descriptor.settings)
       and persists it. Encoding: TOGGLE 1 byte (0 or 1); INT, SLIDER int32_t;
       CHOICE uint32_t; TEXT UTF-8; TEXT_LIST UTF-8 items separated by '\n'.
       ctx->settings shows the new value from the next call. */
    mc_status(MC_CALL* set_setting)(mc_ctx* ctx, uint32_t index, const void* value, uint32_t bytes);
    /* Copies a stored value into out. *bytes receives the value's size;
       MC_E_TRUNCATED when it exceeds capacity (nothing is copied), MC_E_NOT_FOUND
       when the key has no value. */
    mc_status(MC_CALL* store_get)(mc_ctx* ctx, mc_str key, void* out, uint32_t capacity, uint32_t* bytes);
    /* Sets a value (0..MC_STORE_VALUE_MAX bytes); MC_E_BUDGET past the key or
       total limits. Persisted shortly after the last write and on disable. */
    mc_status(MC_CALL* store_set)(mc_ctx* ctx, mc_str key, const void* value, uint32_t bytes);
    /* MC_E_NOT_FOUND when the key had no value. */
    mc_status(MC_CALL* store_remove)(mc_ctx* ctx, mc_str key);
    /* A short in-app notice, 1..MC_TOAST_TEXT_MAX bytes; flags must be 0. */
    mc_status(MC_CALL* toast)(mc_ctx* ctx, mc_str text, uint32_t tone, uint32_t flags);
};

/* True when the host's table contains `field` (for entries above the plugin's api_level). */
#define MC_API_HAS(ctx, field) ((ctx)->api->size >= offsetof(mc_api, field) + sizeof((ctx)->api->field))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAICORD_PLUGIN_ABI_H */

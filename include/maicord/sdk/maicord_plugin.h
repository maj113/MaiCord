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
 *   counts, settings offsets, hooks, commands, menus, buttons, network hosts
 *   and api_level. Everything the plugin does is declared there: metadata,
 *   permissions, settings schema, state size, hooks, slash commands, context
 *   menu items, buttons and the HTTPS hosts it may reach.
 *   maicord/sdk/plugin.hpp generates the descriptor from C++ declarations; C
 *   plugins write it by hand. maicord/sdk/README.md is the API reference.
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
 *   function exposes tokens, credentials, cookies, Discord's REST API or
 *   network access beyond the declared HTTPS hosts.
 */
#ifndef MAICORD_PLUGIN_H
#define MAICORD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_ABI 6u
#define MC_API_LEVEL 2u /* highest mc_api level this header describes */
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

/* A list of Discord ids (USER_LIST settings). */
typedef struct mc_ids {
    const uint64_t* items;
    uint32_t count;
    uint32_t reserved; /* 0 */
} mc_ids;

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
    MC_E_RATE_LIMITED = 10, /* the rate limit is exhausted; retry later */
    MC_E_GESTURE = 11       /* needs a user gesture (or, for writes, MC_PERM_AUTOMATED_ACTIONS) */
};

/* Permissions the user approves at enable. Hooks, buttons and network hosts
   imply theirs and the host ORs them into descriptor.permissions, so plugins
   list only what their API calls need. */
#define MC_PERM_MESSAGE_SEND (1ull << 0)    /* PRE_SEND, PRE_EDIT, PRE_TYPING: change what you send */
#define MC_PERM_READ_MESSAGES (1ull << 1)   /* message hooks, popover buttons, message events: read messages */
#define MC_PERM_MESSAGE_HISTORY (1ull << 2) /* MESSAGE_DELETED, MESSAGE_EDITED: keep them visible this session */
#define MC_PERM_READ_ACTIVITY (1ull << 3)   /* other events: presence, voice, servers, members, friends */
#define MC_PERM_NETWORK (1ull << 4)         /* descriptor.network: HTTPS to the declared hosts */
#define MC_PERM_COMPOSER (1ull << 5)        /* read and change the composer, start replies and edits */
#define MC_PERM_ACT_MESSAGES (1ull << 6)    /* send, edit and delete your messages, react, mark read */
#define MC_PERM_ACT_PRESENCE (1ull << 7)    /* set your activity and custom status */
#define MC_PERM_CLIPBOARD (1ull << 8)       /* copy text */
/* The official catalog refuses these; sideloaded plugins may declare them and
   the consent dialog lists them separately. */
#define MC_PERM_AUTOMATED_ACTIONS (1ull << 9) /* writes without a gesture; implies ACT_MESSAGES */
#define MC_PERM_PERSISTENT_LOG (1ull << 10)   /* restore kept messages from the plugin's own log (keep_restore) */
#define MC_PERM_KNOWN ((1ull << 11) - 1u)

/* Tones map to theme roles; plugins never choose colours. */
enum {
    MC_TONE_NEUTRAL = 0,
    MC_TONE_ACCENT = 1,
    MC_TONE_POSITIVE = 2,
    MC_TONE_WARNING = 3,
    MC_TONE_CRITICAL = 4,
    MC_TONE_ROLE = 5 /* the author's role colour; name prefix and suffix only */
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
    MC_SET_SECRET = 6,     /* mc_str, at most max_bytes; no default; sealed at rest, shown masked */
    MC_SET_COLOUR = 7,     /* uint32_t 0..0xFFFFFF (0xRRGGBB); default_int */
    MC_SET_KEYBIND = 8,    /* uint32_t MC_KEY(vk, modifiers) with Ctrl or Alt, or F1..F24 alone;
                              0 = unbound; default_int; pressing it calls MC_HOOK_KEYBIND */
    MC_SET_TEXT_LIST = 9,  /* mc_list of mc_str, each at most max_bytes; defaults in items */
    MC_SET_CHANNEL = 10,   /* uint64_t channel id, 0 = none */
    MC_SET_USER_LIST = 11, /* mc_ids of at most MC_SETTING_ITEM_MAX non-zero user ids; empty by default */
    MC_SET_HEADER = 12     /* a section label; no value, offset ignored */
};
#define MC_SET_HIDDEN 1u    /* not shown in the settings page */
#define MC_SET_DANGER 2u    /* critical tone */
#define MC_SET_MULTILINE 4u /* TEXT: multi-line input */

/* MC_SET_KEYBIND values: a Windows virtual-key code and modifiers. */
#define MC_KEY_CTRL 0x10000u
#define MC_KEY_SHIFT 0x20000u
#define MC_KEY_ALT 0x40000u
#define MC_KEY(vk, modifiers) ((uint32_t)(vk) | (uint32_t)(modifiers))

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
    uint16_t visible_if;    /* 1-based index of a TOGGLE that must be on or a CHOICE that must equal
                               visible_value; 0 = always */
    uint16_t max_bytes;     /* TEXT, SECRET, TEXT_LIST items; 0 = MC_SETTING_TEXT_MAX */
    uint32_t visible_value; /* visible_if names a CHOICE: the index it must have; else 0 */
} mc_setting_def;

/* ---- Views ---------------------------------------------------------------- */
/* Host-written and valid for the call, strings included. Views only grow by
   appending fields. */

enum {
    MC_RELATIONSHIP_NONE = 0,
    MC_RELATIONSHIP_FRIEND = 1,
    MC_RELATIONSHIP_BLOCKED = 2,
    MC_RELATIONSHIP_INCOMING = 3, /* friend request to you */
    MC_RELATIONSHIP_OUTGOING = 4  /* friend request from you */
};
enum {
    MC_PRESENCE_UNKNOWN = 0,
    MC_PRESENCE_ONLINE = 1,
    MC_PRESENCE_IDLE = 2,
    MC_PRESENCE_DND = 3,
    MC_PRESENCE_OFFLINE = 4
};

/* mc_message.flags */
#define MC_MESSAGE_OWN 1u
#define MC_MESSAGE_BOT 2u
#define MC_MESSAGE_EDITED 4u
#define MC_MESSAGE_PINNED 8u
#define MC_MESSAGE_REPLY 16u
#define MC_MESSAGE_MEDIA 32u    /* attachments or embeds with images or video */
#define MC_MESSAGE_SYSTEM 64u   /* joins, pins, calls and other non-user messages */
#define MC_MESSAGE_DELETED 128u /* a kept tombstone */
#define MC_MESSAGE_LOCAL 256u   /* a local message only you see */
#define MC_MESSAGE_PRIVATE 512u /* in a DM or group DM */
#define MC_MESSAGE_NSFW 1024u   /* in an age-restricted channel */
#define MC_MESSAGE_CACHED 2048u /* content and author come from MaiCord's cache (MESSAGE_DELETED) */

typedef struct mc_message {
    uint64_t message_id;
    uint64_t channel_id;
    uint64_t guild_id;     /* 0 in DMs */
    uint64_t author_id;
    uint64_t reply_to;     /* the replied-to message, or 0 */
    uint64_t timestamp_ms; /* sent, Unix milliseconds */
    uint64_t edited_ms;    /* 0 when never edited */
    mc_str content;        /* markdown as sent; DECORATE_MESSAGE: only with MC_NEED_CONTENT */
    mc_str author_name;    /* the name shown in this channel; DECORATE_MESSAGE: only with MC_NEED_AUTHOR */
    uint32_t flags;        /* MC_MESSAGE_* */
    uint32_t type;         /* Discord message type */
    uint32_t revision;     /* changes whenever what the host shows for the message changes */
    uint32_t relationship; /* yours to the author: MC_RELATIONSHIP_* */
    uint32_t attachment_count;
    uint32_t embed_count;
} mc_message;

/* mc_user.where: the surface asking (DECORATE_USER); 0 from read_user. */
enum { MC_USER_MEMBER_ROW = 1, MC_USER_MESSAGE_AUTHOR = 2, MC_USER_PROFILE = 3 };
#define MC_USER_BOT 1u
#define MC_USER_SELF 2u
/* mc_user.platforms: where the user is online. */
#define MC_PLATFORM_DESKTOP 1u
#define MC_PLATFORM_MOBILE 2u
#define MC_PLATFORM_WEB 4u

typedef struct mc_user {
    uint64_t user_id;
    uint64_t guild_id;   /* the guild the name and roles come from, or 0 */
    uint64_t channel_id; /* the channel shown, or 0 */
    uint32_t where;      /* MC_USER_* */
    uint32_t flags;      /* MC_USER_BOT, MC_USER_SELF */
    uint32_t relationship;
    uint32_t presence;  /* MC_PRESENCE_*; UNKNOWN without MC_PERM_READ_ACTIVITY */
    uint32_t platforms; /* MC_PLATFORM_*; 0 without MC_PERM_READ_ACTIVITY */
    uint32_t reserved;
    mc_str name;     /* display name: nickname, global name or username */
    mc_str username; /* the unique username */
} mc_user;

#define MC_CHANNEL_NSFW 1u
#define MC_CHANNEL_UNREAD 2u
#define MC_CHANNEL_MUTED 4u
#define MC_CHANNEL_PRIVATE 8u  /* a DM or group DM */
#define MC_CHANNEL_CAN_SEND 16u /* you may send messages here */

typedef struct mc_channel {
    uint64_t channel_id;
    uint64_t guild_id;         /* 0 for DMs */
    uint64_t parent_id;        /* category or parent channel, or 0 */
    uint64_t self_permissions; /* yours, Discord permission bits; 0 in DMs */
    uint32_t type;             /* Discord channel type */
    uint32_t flags;            /* MC_CHANNEL_* */
    uint32_t mention_count;
    uint32_t reserved;
    mc_str name; /* DMs: the other user's name */
} mc_channel;

#define MC_GUILD_UNREAD 1u
#define MC_GUILD_MUTED 2u

typedef struct mc_guild {
    uint64_t guild_id;
    uint64_t owner_id;
    uint64_t self_permissions; /* yours, Discord permission bits */
    uint32_t flags;            /* MC_GUILD_* */
    uint32_t mention_count;
    uint32_t member_count; /* 0 when unknown */
    uint32_t online_count; /* 0 when unknown */
    mc_str name;
    /* Level 2. read_guild only: 0 in DECORATE_GUILD and when unknown. */
    uint32_t boost_count;        /* server boosts */
    uint32_t boost_tier;         /* 0..3 */
    uint32_t verification_level; /* Discord's 0 (none) .. 4 (highest) */
    uint32_t channel_count;      /* channels and categories you can see */
    uint32_t role_count;         /* @everyone included */
    uint32_t emoji_count;
} mc_guild;

/* mc_header.where (DECORATE_HEADER) */
enum { MC_HEADER_CHANNEL = 1, MC_HEADER_MEMBER_LIST = 2 };

typedef struct mc_header {
    uint64_t channel_id;
    uint64_t guild_id;
    uint32_t where;        /* MC_HEADER_* */
    uint32_t member_count; /* 0 when unknown */
    uint32_t online_count; /* 0 when unknown */
    uint32_t reserved;
    mc_str name; /* the channel's name */
} mc_header;

/* read_open_channel() (level 2): what the chat view shows now. */
typedef struct mc_open_channel {
    uint64_t channel_id; /* the open channel */
    uint64_t guild_id;   /* 0 in DMs */
    uint64_t reply_to;   /* the composer's reply target, or 0 */
    uint64_t edit_id;    /* the message being edited, or 0 */
} mc_open_channel;

/* read_messages() (level 2): one message MaiCord holds, without its text. */
#define MC_READ_MESSAGES_MAX 64u
typedef struct mc_message_ref {
    uint64_t message_id;
    uint64_t author_id;
    uint64_t timestamp_ms;
    uint32_t flags; /* MC_MESSAGE_* */
    uint32_t type;  /* Discord message type */
} mc_message_ref;

/* ---- Message pipeline ----------------------------------------------------- */

/* mc_draft.flags (PRE_SEND); in and out. */
#define MC_DRAFT_SILENT 1u /* @silent: no push notification */
#define MC_DRAFT_SUPPRESS_EMBEDS 2u
#define MC_DRAFT_REPLY_PING 4u /* the reply pings the replied-to author */

/* mc_draft.verdict, set by the plugin; the host sets MC_DRAFT_KEEP first. */
enum {
    MC_DRAFT_KEEP = 0,    /* unchanged */
    MC_DRAFT_REPLACE = 1, /* buffer[0, replaced_bytes) replaces text */
    MC_DRAFT_CANCEL = 2,  /* don't send or save; the composer keeps the text */
    MC_DRAFT_CONFIRM = 3  /* PRE_SEND: ask first; buffer[0, replaced_bytes) is the question */
};

#define MC_DRAFT_TEXT_MAX 4000u  /* buffer_capacity is at least this */
#define MC_CONFIRM_TEXT_MAX 256u /* MC_DRAFT_CONFIRM question */

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
    uint32_t replaced_bytes;  /* out, with MC_DRAFT_REPLACE or CONFIRM: valid UTF-8, not empty */
    uint32_t flags;           /* in/out: MC_DRAFT_* (PRE_SEND); PRE_EDIT ignores changes */
    uint32_t verdict;         /* out: MC_DRAFT_* */
    uint64_t reply_author_id; /* level 2. PRE_SEND: the author of reply_to, or 0 when unknown */
} mc_draft;

/* MESSAGE_CLICK. hook.filter: the clicks the hook wants, 0 = all. */
enum { MC_CLICK_LEFT = 0, MC_CLICK_MIDDLE = 1 };
#define MC_CLICK_FILTER_LEFT 1u
#define MC_CLICK_FILTER_DOUBLE 2u /* left double click */
#define MC_CLICK_FILTER_MIDDLE 4u
#define MC_MOD_SHIFT 1u
#define MC_MOD_CTRL 2u
#define MC_MOD_ALT 4u
typedef struct mc_click {
    uint32_t button;    /* MC_CLICK_LEFT, MC_CLICK_MIDDLE */
    uint32_t count;     /* 1, or 2 for a double click */
    uint32_t modifiers; /* MC_MOD_* */
    uint32_t reserved;
} mc_click;
enum { MC_CLICK_PASS = 0, MC_CLICK_HANDLED = 1 /* skip the host's own handling */ };

/* MESSAGE_DELETED: called before the host forgets the message. MESSAGE_EDITED:
   MC_KEEP keeps the previous content in the message's edit history. Any
   plugin's MC_KEEP wins. Kept messages live in a session store. */
enum { MC_DROP = 0, MC_KEEP = 1 };
#define MC_KEEP_MESSAGES_MAX 500u
#define MC_KEEP_PER_CHANNEL_MAX 32u
#define MC_KEEP_EDITS_MAX 8u
#define MC_KEEP_CONTENT_MAX 4000u
#define MC_KEEP_RESTORE_MAX 64u /* items per keep_restore */

#define MC_KEPT_DELETED 1u
/* keep_info() */
typedef struct mc_kept_info {
    uint32_t flags;      /* MC_KEPT_DELETED */
    uint32_t edit_count; /* older versions held */
    uint64_t author_id;
    uint64_t timestamp_ms;
    uint64_t edited_ms;
} mc_kept_info;
/* keep_restore(): re-seeds the session store, e.g. from the plugin's own store. */
typedef struct mc_kept {
    uint32_t flags; /* MC_KEPT_DELETED */
    uint32_t edit_count;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t author_id;
    uint64_t timestamp_ms;
    uint64_t edited_ms;
    mc_str content;
    mc_str author_name;
    const mc_str* edits; /* edit_count older versions, most recent first */
} mc_kept;

/* PRE_TYPING verdicts */
enum { MC_ALLOW = 0, MC_SUPPRESS = 1 };

/* EVENTS: hook.filter is a non-zero mask of MC_EVENT_BIT(kind); OVERFLOW is
   always delivered. Batches hold at most MC_EVENT_BATCH_MAX events. Unless
   noted, ids that don't apply are 0 and strings empty. */
enum {
    MC_EVENT_MESSAGE_CREATE = 0,   /* text: content; name: author */
    MC_EVENT_MESSAGE_UPDATE = 1,   /* text: new content when the update carries it (MC_EVENT_TEXT) */
    MC_EVENT_MESSAGE_DELETE = 2,   /* text, name, user_id: the last cached copy (MC_EVENT_CACHED); MC_EVENT_BULK */
    MC_EVENT_TYPING = 3,           /* user_id is typing in channel_id */
    MC_EVENT_REACTION_ADD = 4,     /* value: emoji id (0: Unicode); text: emoji; user_id: who reacted */
    MC_EVENT_REACTION_REMOVE = 5,
    MC_EVENT_PRESENCE = 6,         /* value: MC_PRESENCE_* */
    MC_EVENT_VOICE_STATE = 7,      /* channel_id: the voice channel, 0 after leaving */
    MC_EVENT_CHANNEL_CREATE = 8,   /* value: channel type */
    MC_EVENT_CHANNEL_UPDATE = 9,
    MC_EVENT_CHANNEL_DELETE = 10,  /* also when you are removed from a group DM; MC_EVENT_OWN: you left it here */
    MC_EVENT_GUILD_CREATE = 11,
    MC_EVENT_GUILD_UPDATE = 12,
    MC_EVENT_GUILD_DELETE = 13,    /* you left or were removed, or it was deleted (not outages);
                                      MC_EVENT_OWN: you left or deleted it here */
    MC_EVENT_MEMBER_ADD = 14,
    MC_EVENT_MEMBER_UPDATE = 15,
    MC_EVENT_MEMBER_REMOVE = 16,
    MC_EVENT_RELATIONSHIP_ADD = 17,    /* user_id: the other user; value: MC_RELATIONSHIP_* */
    MC_EVENT_RELATIONSHIP_REMOVE = 18, /* value: the relationship that ended; MC_EVENT_OWN: you ended it here */
    MC_EVENT_KIND_COUNT = 19,
    MC_EVENT_OVERFLOW = 63 /* value: events lost */
};
#define MC_EVENT_BIT(kind) (1ull << (kind))
/* Message, typing and reaction kinds imply MC_PERM_READ_MESSAGES; the rest MC_PERM_READ_ACTIVITY. */
#define MC_EVENT_MESSAGE_KINDS ((1ull << (MC_EVENT_REACTION_REMOVE + 1)) - 1u)
#define MC_EVENT_KINDS ((1ull << MC_EVENT_KIND_COUNT) - 1u)
#define MC_EVENT_BATCH_MAX 64u
/* mc_event.flags */
#define MC_EVENT_OWN 1u    /* your own message, reaction, presence or voice state; for removals, your
                              own action in this MaiCord (unfriend, leave, delete) */
#define MC_EVENT_BOT 2u
#define MC_EVENT_BULK 4u   /* part of a bulk delete */
#define MC_EVENT_CACHED 8u /* text and name come from MaiCord's cache */
#define MC_EVENT_TEXT 16u  /* text is present */
#define MC_EVENT_TRUNCATED 32u /* text was cut */

typedef struct mc_event {
    uint32_t kind;  /* MC_EVENT_* */
    uint32_t flags; /* MC_EVENT_* flags */
    uint64_t guild_id;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t user_id;
    uint64_t value;   /* per kind */
    uint64_t time_ms; /* when MaiCord received it, Unix milliseconds */
    mc_str text;
    mc_str name;
} mc_event;

/* ---- Decorations ---------------------------------------------------------- */
/* Decorate hooks run in the host's prepare pass, never while painting; the
   result is cached by entity revision until invalidate() or decorate_push().
   The host zeroes the mc_decoration and copies its strings after the call, so
   they must not point at the hook's stack (plugin.hpp's Decoration copies
   them into storage that lasts until the next entry point). A
   hook that takes over 1 ms, fails, or writes an invalid decoration decorates
   nothing for that revision. Badges show on message author lines, member
   rows and both headers; channel rows and server icons show the first one.
   BADGE and ICON nodes need text or an icon; LABEL, LINK and BUTTON need text. */

/* hook.flags of DECORATE_MESSAGE: fields the hook reads. */
#define MC_NEED_CONTENT 1u
#define MC_NEED_AUTHOR 2u
/* hook.filter of DECORATE_MESSAGE: the hook sees only messages matching every
   bit set. DECORATE_USER: a mask of (1u << MC_USER_*) surfaces, 0 = all. */
#define MC_FILTER_GUILD_ONLY 1u
#define MC_FILTER_DM_ONLY 2u
#define MC_FILTER_HAS_MEDIA 4u
#define MC_FILTER_NOT_OWN 8u

enum {
    MC_NODE_LABEL = 0,
    MC_NODE_ICON = 1,  /* tooltip is its accessible name */
    MC_NODE_BADGE = 2, /* compact pill: text and/or icon */
    MC_NODE_BUTTON = 3, /* tag reported to NODE_ACTION */
    MC_NODE_LINK = 4,  /* value: https URL, opened with the host's link confirmation */
    MC_NODE_CARD_BUTTON = 5 /* mc_node_action.kind only */
};
#define MC_NODE_TEXT_MAX 256u /* labels and links; badges and buttons 32 */
#define MC_NODE_SHORT_MAX 32u
#define MC_NODE_TOOLTIP_MAX 64u
#define MC_URL_MAX 512u

typedef struct mc_node {
    uint32_t kind; /* MC_NODE_* */
    uint32_t tone; /* MC_TONE_* */
    mc_str text;
    mc_str icon;    /* host icon name, e.g. "lucide/star"; unknown names show a placeholder */
    mc_str tooltip; /* at most MC_NODE_TOOLTIP_MAX */
    mc_str value;   /* LINK: the URL */
    uint64_t tag;   /* BUTTON: 1..2^63-1 */
} mc_node;

#define MC_CARD_BUTTONS 2u
#define MC_CARD_TITLE_MAX 64u
#define MC_CARD_TEXT_MAX 1024u

typedef struct mc_card_button {
    mc_str label; /* 1..MC_NODE_SHORT_MAX */
    uint64_t tag; /* 1..2^63-1; reported to NODE_ACTION as MC_NODE_CARD_BUTTON */
    uint32_t tone;
    uint32_t reserved;
} mc_card_button;

/* A rich block: message accessories and local messages. */
typedef struct mc_card {
    mc_str title;     /* at most MC_CARD_TITLE_MAX; a card needs a title or text */
    mc_str text;      /* plain text with line feeds, at most MC_CARD_TEXT_MAX */
    mc_str caption;   /* at most MC_CARD_TITLE_MAX */
    mc_str link;      /* optional https URL the title opens */
    mc_str thumbnail; /* optional https image on Discord's CDN or a declared, approved network host */
    mc_card_button buttons[MC_CARD_BUTTONS];
    uint32_t button_count;
    uint32_t tone; /* accent bar */
} mc_card;

enum { MC_VIS_NORMAL = 0, MC_VIS_COLLAPSED = 1, MC_VIS_HIDDEN = 2 };
enum { MC_MEDIA_SHOW = 0, MC_MEDIA_BLUR = 1, MC_MEDIA_HIDE = 2 };
#define MC_DECO_REPLACE_CONTENT 1u /* show content instead of the message text */
#define MC_DECORATION_BADGES 4u
#define MC_DECORATION_ACCESSORIES 8u
#define MC_DECORATION_CONTENT_MAX 2000u

typedef struct mc_decoration {
    uint32_t visibility; /* MC_VIS_*; messages only */
    uint32_t media;      /* MC_MEDIA_*; messages only */
    uint32_t name_tone;  /* MC_TONE_* for the prefix and suffix */
    uint32_t flags;      /* MC_DECO_* */
    mc_str collapsed_label; /* at most 64; empty = the host's text */
    mc_str name_prefix;     /* plain text, at most MC_NODE_SHORT_MAX */
    mc_str name_suffix;     /* plain text, at most MC_NODE_SHORT_MAX */
    mc_str content;         /* with MC_DECO_REPLACE_CONTENT: display-only markdown */
    uint32_t badge_count;
    uint32_t accessory_count;
    uint32_t has_card;
    uint32_t reserved;
    mc_node badges[MC_DECORATION_BADGES];           /* by the name, row or icon */
    mc_node accessories[MC_DECORATION_ACCESSORIES]; /* rows below a message; profile section rows */
    mc_card card;                                   /* messages: below the accessories */
} mc_decoration;

/* NODE_ACTION: a BUTTON node or card button was clicked (a gesture). */
typedef struct mc_node_action {
    uint64_t tag;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t user_id;
    uint64_t guild_id;
    uint32_t source; /* the MC_HOOK_DECORATE_* kind, or 0 for a pushed decoration or local message */
    uint32_t kind;   /* MC_NODE_BUTTON or MC_NODE_CARD_BUTTON */
} mc_node_action;

/* invalidate(what, id) */
enum {
    MC_INVALIDATE_MESSAGE = 1,
    MC_INVALIDATE_USER = 2,
    MC_INVALIDATE_CHANNEL = 3,
    MC_INVALIDATE_GUILD = 4,
    MC_INVALIDATE_HEADER = 5,
    MC_INVALIDATE_BUTTONS = 6
};

/* ---- Buttons (descriptor.buttons) ------------------------------------------ */

enum {
    MC_SURFACE_CHAT_BAR = 1,
    MC_SURFACE_MESSAGE_POPOVER = 2, /* two visible per message; more go into a "Plugins" submenu */
    MC_SURFACE_ACCOUNT_PANEL = 3,
    MC_SURFACE_HEADER = 4,
    MC_SURFACE_SERVER_LIST_TOP = 5,
    MC_SURFACE_SERVER_LIST_BOTTOM = 6
};
#define MC_BUTTON_TOGGLE 1u /* shows its active state */
#define MC_BUTTON_DANGER 2u
#define MC_BUTTON_MAX 16u
#define MC_BUTTON_LABEL_MAX 64u

typedef struct mc_button_event {
    uint64_t channel_id;        /* the open channel, or 0 */
    uint64_t guild_id;          /* 0 in DMs */
    const mc_message* message;  /* MESSAGE_POPOVER: the message; else null */
    uint32_t button;            /* index in descriptor.buttons */
    uint32_t reserved;
} mc_button_event;

typedef uint32_t(MC_CALL* mc_button_visible_fn)(mc_ctx* ctx, const mc_button_event* event); /* non-zero: shown */
typedef mc_status(MC_CALL* mc_button_fn)(mc_ctx* ctx, const mc_button_event* event);          /* gesture */

typedef struct mc_button_def {
    uint32_t surface; /* MC_SURFACE_* */
    uint32_t flags;   /* MC_BUTTON_TOGGLE, MC_BUTTON_DANGER */
    mc_str label;     /* 1..MC_BUTTON_LABEL_MAX: tooltip and accessible name */
    mc_str icon;      /* host icon name */
    mc_button_visible_fn visible; /* optional */
    mc_button_fn click;
} mc_button_def;

/* button_state() */
#define MC_BUTTON_ACTIVE 1u
#define MC_BUTTON_DISABLED 2u
#define MC_BUTTON_HIDDEN 4u
typedef struct mc_button_state {
    uint32_t flags;       /* MC_BUTTON_ACTIVE, MC_BUTTON_DISABLED, MC_BUTTON_HIDDEN */
    uint32_t badge_count; /* 0 = none; above 99 shows 99+ */
    mc_str tooltip;       /* empty = the label */
    mc_str icon;          /* empty = the declared icon */
} mc_button_state;

/* ---- Context menus (descriptor.menus) ---------------------------------------- */

#define MC_MENU_USER 0x1u
#define MC_MENU_MESSAGE 0x2u
#define MC_MENU_CHANNEL 0x4u
#define MC_MENU_GUILD 0x8u
#define MC_MENU_LINK 0x10u
#define MC_MENU_IMAGE 0x20u /* images and other attachments */
#define MC_MENU_EMOJI 0x40u
#define MC_MENU_TARGETS 0x7Fu      /* the targets this ABI implements */
#define MC_MENU_ROLE 0x80u         /* reserved: refused */
#define MC_MENU_VOICE_USER 0x100u  /* reserved: refused */

/* mc_menu_def.flags */
#define MC_MENU_CHECKBOX 1u
#define MC_MENU_DANGER 2u
#define MC_MENU_SUBMENU 4u /* opens its children; no select */
/* What visible returns; without a visible function an item is shown. */
#define MC_MENU_HIDE 0u
#define MC_MENU_SHOW 1u
#define MC_MENU_DISABLED 2u
#define MC_MENU_CHECKED 4u
/* mc_menu_target.flags */
#define MC_MENU_OWN 1u      /* your message or yourself */
#define MC_MENU_PRIVATE 2u  /* in a DM or group DM */
#define MC_MENU_ANIMATED 4u /* an animated emoji */

#define MC_MENU_MAX 32u
#define MC_MENU_LABEL_MAX 64u

typedef struct mc_menu_target {
    uint32_t kind;  /* one MC_MENU_* bit */
    uint32_t flags; /* MC_MENU_OWN, MC_MENU_PRIVATE, MC_MENU_ANIMATED */
    uint64_t user_id;
    uint64_t channel_id;
    uint64_t guild_id;
    uint64_t message_id;
    uint64_t item_id;          /* EMOJI: the emoji (0: Unicode); IMAGE: the attachment */
    mc_str url;                /* LINK, IMAGE: the URL; EMOJI: its image */
    mc_str text;               /* LINK: link text; IMAGE: file name; EMOJI: name */
    mc_str content_type;       /* IMAGE */
    uint32_t attachment_index; /* IMAGE: its position in the message */
    uint32_t width, height;    /* IMAGE; 0 when unknown */
    uint32_t reserved;
} mc_menu_target;

typedef uint32_t(MC_CALL* mc_menu_visible_fn)(mc_ctx* ctx, const mc_menu_target* target); /* MC_MENU_* bits */
typedef mc_status(MC_CALL* mc_menu_fn)(mc_ctx* ctx, const mc_menu_target* target);          /* gesture */

typedef struct mc_menu_def {
    mc_str label;     /* 1..MC_MENU_LABEL_MAX */
    uint32_t targets; /* MC_MENU_* mask; children: a subset of the parent's */
    uint32_t flags;   /* MC_MENU_CHECKBOX, MC_MENU_DANGER, MC_MENU_SUBMENU */
    uint32_t parent;  /* 0, or the 1-based index of an earlier SUBMENU item (one level) */
    uint32_t reserved;
    mc_menu_visible_fn visible; /* optional; runs when the menu opens */
    mc_menu_fn select;          /* null exactly for SUBMENU items */
} mc_menu_def;

/* ---- Slash commands (descriptor.commands) ------------------------------------- */

enum { MC_ARG_TEXT = 1, MC_ARG_INT = 2, MC_ARG_BOOL = 3, MC_ARG_USER = 4, MC_ARG_CHANNEL = 5 };
#define MC_ARG_REQUIRED 1u    /* required arguments come first */
#define MC_COMMAND_GUILD_ONLY 1u
#define MC_COMMAND_MAX 16u
#define MC_COMMAND_ARGS_MAX 8u
#define MC_COMMAND_NAME_MAX 32u        /* names: 1..32 bytes of [a-z0-9_-] */
#define MC_COMMAND_DESCRIPTION_MAX 100u /* descriptions: 1..100 bytes */
#define MC_ARG_CHOICES_MAX 25u
#define MC_REPLY_TEXT_MAX 4000u

typedef struct mc_arg_def {
    mc_str name;
    mc_str description;
    uint32_t type;  /* MC_ARG_* */
    uint32_t flags; /* MC_ARG_REQUIRED */
    int64_t minimum, maximum; /* INT: value range; TEXT: length in characters; both 0 = unbounded */
    const mc_str* choices;    /* TEXT: the allowed values, or null */
    uint32_t choice_count;
    uint32_t reserved;
} mc_arg_def;

/* One per mc_arg_def, in order. */
typedef struct mc_arg {
    uint32_t type;
    uint32_t present; /* 0 for an omitted optional argument */
    mc_str text;      /* TEXT */
    int64_t integer;  /* INT; BOOL 0 or 1 */
    uint64_t id;      /* USER, CHANNEL */
} mc_arg;

enum {
    MC_REPLY_NONE = 0,
    MC_REPLY_SEND = 1,   /* send the reply text through PRE_SEND; needs MC_PERM_MESSAGE_SEND */
    MC_REPLY_INSERT = 2, /* put it in the composer */
    MC_REPLY_LOCAL = 3,  /* a local message only you see, in reply_tone */
    MC_REPLY_TOAST = 4   /* a toast in reply_tone */
};

typedef struct mc_command_call {
    uint64_t channel_id;
    uint64_t guild_id; /* 0 in DMs */
    const mc_arg* args;
    uint32_t arg_count;
    uint32_t reply_kind; /* out: MC_REPLY_* */
    char* reply;         /* out: host-owned buffer */
    uint32_t reply_capacity;
    uint32_t reply_bytes; /* out */
    uint32_t reply_tone;  /* out: MC_TONE_* */
    uint32_t reserved;
} mc_command_call;

typedef mc_status(MC_CALL* mc_command_fn)(mc_ctx* ctx, mc_command_call* call); /* gesture */

typedef struct mc_command_def {
    mc_str name;
    mc_str description;
    const mc_arg_def* args;
    uint32_t arg_count;
    uint32_t flags; /* MC_COMMAND_GUILD_ONLY */
    mc_command_fn run;
} mc_command_def;

/* ---- Network ---------------------------------------------------------------- */
/* descriptor.network lists the hosts http() may reach: lower-case ASCII
   names, exact or "*.example.com", never Discord's. A non-empty list implies
   MC_PERM_NETWORK and needs a MC_HOOK_COMPLETION hook; the consent dialog
   shows every host. Requests are HTTPS only, with no cookies and no
   redirects off the list; responses arrive as MC_COMPLETION_HTTP. */

#define MC_NETWORK_HOSTS_MAX 8u
#define MC_HOST_MAX 253u
#define MC_HTTP_URL_MAX 2048u /* https://host/path?query; no user info, port or fragment */
#define MC_HTTP_HEADERS_MAX 8u
#define MC_HTTP_HEADER_VALUE_MAX 1024u
#define MC_HTTP_BODY_MAX 4096u /* POST body */
#define MC_HTTP_RESPONSE_MAX (1u << 20)
#define MC_HTTP_REDIRECTS_MAX 3u
#define MC_HTTP_REQUESTS_PER_MINUTE 30u
#define MC_HTTP_PENDING_MAX 4u /* in flight per plugin */

enum { MC_HTTP_GET = 1, MC_HTTP_POST = 2 };

typedef struct mc_http_header {
    mc_str name;  /* a token; not Host, Cookie, Content-Length, Connection, Transfer-Encoding,
                     Accept-Encoding or User-Agent */
    mc_str value; /* at most MC_HTTP_HEADER_VALUE_MAX */
} mc_http_header;

typedef struct mc_http_request {
    uint32_t method; /* MC_HTTP_* */
    uint32_t flags;  /* 0 */
    mc_str url;      /* https:// on a declared host, at most MC_HTTP_URL_MAX */
    const mc_http_header* headers;
    uint32_t header_count;
    uint32_t reserved;
    const void* body;            /* POST only */
    uint32_t body_bytes;         /* 0..MC_HTTP_BODY_MAX; 0 for GET */
    uint32_t max_response_bytes; /* 0 = MC_HTTP_RESPONSE_MAX */
} mc_http_request;

/* mc_completion.error for MC_COMPLETION_HTTP */
enum {
    MC_HTTP_OK = 0,           /* a response arrived; see status */
    MC_HTTP_E_NETWORK = 1,    /* resolve, connect, TLS, timeout or a malformed response */
    MC_HTTP_E_TOO_LARGE = 2,  /* the response exceeded max_response_bytes */
    MC_HTTP_E_REDIRECT = 3,   /* a redirect off the list, not HTTPS, to Discord, or too many */
    MC_HTTP_E_DENIED = 4      /* the user revoked the host before the request ran */
};

/* ---- Feedback: banners and modals ------------------------------------------------ */

typedef struct mc_banner {
    uint32_t tone; /* MC_TONE_NEUTRAL or MC_TONE_WARNING */
    uint32_t reserved;
    uint64_t tag;  /* reported in MC_COMPLETION_BANNER */
    mc_str title;  /* 1..96 bytes */
    mc_str text;   /* 0..192 bytes */
    mc_str action; /* 0..32 bytes: the button label; empty = none */
} mc_banner;
enum { MC_BANNER_ACTION = 1, MC_BANNER_DISMISSED = 2 };

/* A host dialog: a confirmation, optionally with one toggle, or a form with
   one text input, optionally with one dropdown. */
#define MC_MODAL_DANGER 1u         /* the accept button is destructive */
#define MC_MODAL_ACCEPT_FOCUSED 2u /* accept has the initial focus */
#define MC_MODAL_MULTILINE 4u      /* the input takes several lines */
#define MC_MODAL_SELECT_MAX 16u

typedef struct mc_modal {
    uint32_t flags;     /* MC_MODAL_* */
    uint32_t has_input; /* 1: a form with a text input */
    mc_str title;       /* 1..64 bytes */
    mc_str body;        /* 0..512 bytes */
    mc_str accept;      /* 0..32 bytes; empty = "OK" */
    mc_str cancel;      /* 0..32 bytes; empty = "Cancel" */
    mc_str input_label;
    mc_str input_initial;
    mc_str input_hint;
    uint32_t input_max_chars; /* 0 = none; else 1..2000 */
    uint32_t toggle_initial;  /* 0 or 1 */
    mc_str toggle_label;      /* confirmation only; empty = no toggle */
    mc_str select_label;      /* form only; empty = no dropdown */
    const mc_str* select_items;
    uint32_t select_count; /* 0..MC_MODAL_SELECT_MAX */
    uint32_t select_initial;
} mc_modal;

/* ---- Completions -------------------------------------------------------------- */
/* Results of asynchronous calls arrive at MC_HOOK_COMPLETION with the
   caller's tag; strings and the body are valid for the call. A completion
   of a call made inside a gesture (PRE_SEND counts as one) carries
   MC_CTX_GESTURE until 5 seconds after that gesture; calls started from that
   completion keep the same deadline. */

enum { MC_COMPLETION_HTTP = 1, MC_COMPLETION_MODAL = 2, MC_COMPLETION_BANNER = 3 };

typedef struct mc_completion {
    uint32_t kind;  /* MC_COMPLETION_* */
    uint32_t error; /* HTTP: MC_HTTP_E_*, else 0 */
    uint64_t tag;
    uint32_t status;      /* HTTP: 200..599, 0 on error */
    uint32_t body_bytes;  /* HTTP */
    const void* body;     /* HTTP: the response; null when body_bytes is 0 */
    uint32_t result;      /* MODAL: 1 accepted, 0 cancelled; BANNER: MC_BANNER_* */
    uint32_t toggle;      /* MODAL: the toggle, 0 or 1 */
    uint32_t select_index; /* MODAL: the dropdown */
    uint32_t reserved;
    mc_str text; /* MODAL: the input text */
} mc_completion;

/* ---- Actions ------------------------------------------------------------------ */
/* Writes (send, edit, react, delete, mark read) need MC_PERM_ACT_MESSAGES and a
   gesture or its completion window; MC_PERM_AUTOMATED_ACTIONS lifts the
   gesture requirement. They're rate-limited per plugin (MC_E_RATE_LIMITED).
   open_url, copy_text and open always need a gesture. */

typedef struct mc_send {
    uint64_t channel_id;
    uint64_t reply_to; /* 0 = not a reply */
    mc_str text;       /* 1..MC_DRAFT_TEXT_MAX; not run through PRE_SEND */
    uint32_t flags;    /* MC_DRAFT_SILENT, MC_DRAFT_SUPPRESS_EMBEDS, MC_DRAFT_REPLY_PING */
    uint32_t reserved;
} mc_send;

#define MC_REACT_REMOVE 1u /* react(): remove your reaction instead */
#define MC_ACTION_TEXT_MAX 4000u /* edit, local messages, copy_text */
enum { MC_READ_CHANNEL = 1, MC_READ_GUILD = 2, MC_READ_ALL = 3 };
enum {
    MC_OPEN_CHANNEL = 1, /* id: channel */
    MC_OPEN_PROFILE = 2  /* id: user; sub: guild or 0 */
};

enum {
    MC_ACTIVITY_PLAYING = 0,
    MC_ACTIVITY_STREAMING = 1,
    MC_ACTIVITY_LISTENING = 2,
    MC_ACTIVITY_WATCHING = 3,
    MC_ACTIVITY_CUSTOM = 4, /* custom status: state and emoji */
    MC_ACTIVITY_COMPETING = 5
};
typedef struct mc_activity {
    uint32_t type; /* MC_ACTIVITY_* */
    uint32_t flags; /* 0 */
    uint64_t application_id; /* optional */
    uint64_t start_ms;       /* optional Unix milliseconds */
    uint64_t end_ms;
    mc_str name;
    mc_str details;
    mc_str state;
    mc_str url;   /* STREAMING */
    mc_str emoji; /* CUSTOM: a Unicode emoji */
} mc_activity;

#define MC_TIMERS_MAX 16u
#define MC_TIMER_INTERVAL_MIN 100u /* milliseconds */

/* ---- Hooks -------------------------------------------------------------------- */

enum {
    MC_HOOK_PRE_SEND = 1,          /* draft: before a message you typed is sent */
    MC_HOOK_PRE_EDIT = 2,          /* draft: before an edit of your message is saved */
    MC_HOOK_MESSAGE_CLICK = 3,     /* click: a message was clicked (gesture) */
    MC_HOOK_MESSAGE_DELETED = 4,   /* deleted: a message was deleted */
    MC_HOOK_MESSAGE_EDITED = 5,    /* edited: a message was edited */
    MC_HOOK_EVENTS = 6,            /* events: filter = MC_EVENT_BIT mask; batched per UI pass */
    MC_HOOK_DECORATE_MESSAGE = 7,  /* decorate_message */
    MC_HOOK_DECORATE_USER = 8,     /* decorate_user: member rows, message authors, profiles */
    MC_HOOK_DECORATE_CHANNEL = 9,  /* decorate_channel: channel and DM rows */
    MC_HOOK_DECORATE_GUILD = 10,   /* decorate_guild: server rail icons */
    MC_HOOK_DECORATE_HEADER = 11,  /* decorate_header: channel header, member-list header */
    MC_HOOK_NODE_ACTION = 12,      /* node_action: a decoration button was clicked (gesture) */
    MC_HOOK_PRE_TYPING = 13,       /* typing: before a typing notice is sent */
    MC_HOOK_SETTING_CHANGED = 14,  /* setting: the user changed a setting on its page (gesture); not set_setting */
    MC_HOOK_KEYBIND = 15,          /* setting: a KEYBIND setting's keys were pressed (gesture) */
    MC_HOOK_TIMER = 16,            /* timer */
    MC_HOOK_COMPLETION = 17,       /* completion */
    MC_HOOK_LAST = MC_HOOK_COMPLETION
};

typedef mc_status(MC_CALL* mc_draft_fn)(mc_ctx* ctx, mc_draft* draft);
typedef uint32_t(MC_CALL* mc_click_fn)(mc_ctx* ctx, const mc_message* message, const mc_click* click);
typedef uint32_t(MC_CALL* mc_deleted_fn)(mc_ctx* ctx, const mc_message* message);
typedef uint32_t(MC_CALL* mc_edited_fn)(mc_ctx* ctx, const mc_message* before, const mc_message* after);
typedef void(MC_CALL* mc_events_fn)(mc_ctx* ctx, const mc_event* events, uint32_t count);
typedef mc_status(MC_CALL* mc_decorate_message_fn)(mc_ctx* ctx, const mc_message* message, mc_decoration* out);
typedef mc_status(MC_CALL* mc_decorate_user_fn)(mc_ctx* ctx, const mc_user* user, mc_decoration* out);
typedef mc_status(MC_CALL* mc_decorate_channel_fn)(mc_ctx* ctx, const mc_channel* channel, mc_decoration* out);
typedef mc_status(MC_CALL* mc_decorate_guild_fn)(mc_ctx* ctx, const mc_guild* guild, mc_decoration* out);
typedef mc_status(MC_CALL* mc_decorate_header_fn)(mc_ctx* ctx, const mc_header* header, mc_decoration* out);
typedef mc_status(MC_CALL* mc_node_action_fn)(mc_ctx* ctx, const mc_node_action* action);
typedef uint32_t(MC_CALL* mc_typing_fn)(mc_ctx* ctx, uint64_t channel_id);
typedef void(MC_CALL* mc_setting_fn)(mc_ctx* ctx, uint32_t setting_index);
typedef void(MC_CALL* mc_timer_fn)(mc_ctx* ctx, uint64_t timer_id);
typedef void(MC_CALL* mc_completion_fn)(mc_ctx* ctx, const mc_completion* completion);

/* The member matching kind is called. */
typedef union mc_hook_fn {
    mc_draft_fn draft;                       /* PRE_SEND, PRE_EDIT */
    mc_click_fn click;                       /* MESSAGE_CLICK */
    mc_deleted_fn deleted;                   /* MESSAGE_DELETED */
    mc_edited_fn edited;                     /* MESSAGE_EDITED */
    mc_events_fn events;                     /* EVENTS */
    mc_decorate_message_fn decorate_message; /* DECORATE_MESSAGE */
    mc_decorate_user_fn decorate_user;       /* DECORATE_USER */
    mc_decorate_channel_fn decorate_channel; /* DECORATE_CHANNEL */
    mc_decorate_guild_fn decorate_guild;     /* DECORATE_GUILD */
    mc_decorate_header_fn decorate_header;   /* DECORATE_HEADER */
    mc_node_action_fn node_action;           /* NODE_ACTION */
    mc_typing_fn typing;                     /* PRE_TYPING */
    mc_setting_fn setting;                   /* SETTING_CHANGED, KEYBIND */
    mc_timer_fn timer;                       /* TIMER */
    mc_completion_fn completion;             /* COMPLETION */
} mc_hook_fn;

typedef struct mc_hook {
    uint32_t kind;   /* MC_HOOK_*; at most one hook per kind */
    uint32_t flags;  /* DECORATE_MESSAGE: MC_NEED_*; otherwise 0 */
    uint64_t filter; /* EVENTS: MC_EVENT_BIT mask, not 0; MESSAGE_CLICK: MC_CLICK_FILTER_*;
                        DECORATE_MESSAGE: MC_FILTER_*; DECORATE_USER: surfaces; otherwise 0 */
    mc_hook_fn fn;
} mc_hook;

/* ---- Descriptor ----------------------------------------------------------- */

struct mc_theme_token; /* reserved: theme_count must be 0 */

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
    uint64_t permissions;                            /* MC_PERM_* the APIs need; the host adds implied ones */
    const mc_str* network;                           /* HTTPS hosts (see Network) */
    uint32_t network_count;  /* 0..MC_NETWORK_HOSTS_MAX */
    uint32_t state_bytes;    /* 0..MC_STATE_MAX */
    uint32_t state_align;    /* power of two, 1..MC_STATE_ALIGN_MAX; 0 when state_bytes is 0 */
    uint32_t settings_bytes; /* 0..MC_SETTINGS_BYTES_MAX */
    uint64_t state_layout;   /* equal across a reload keeps state */
    const mc_setting_def* settings;
    uint32_t setting_count; /* 0..MC_SETTING_MAX */
    uint32_t hook_count;    /* 0..MC_HOOK_LAST */
    const mc_hook* hooks;
    const mc_command_def* commands;
    uint32_t command_count; /* 0..MC_COMMAND_MAX */
    uint32_t menu_count;    /* 0..MC_MENU_MAX */
    const mc_menu_def* menus;
    const mc_button_def* buttons;
    uint32_t button_count; /* 0..MC_BUTTON_MAX */
    uint32_t theme_count;  /* reserved: 0 */
    const struct mc_theme_token* theme;
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

/* Immutable; append-only within ABI 6. Level 1 is everything up to keep_restore; level 2 follows. */
struct mc_api {
    uint32_t size; /* bytes of this table the host provides */
    uint32_t level;
    /* Appends a line to the plugin's log: the Developer console of Settings > Plugins, which holds lines only
       while Developer Mode is on (never message content, tokens or URLs). Longer text is cut at MC_LOG_TEXT_MAX. */
    void(MC_CALL* log)(mc_ctx* ctx, uint32_t level, mc_str text);
    /* Changes one of the plugin's own settings (index into descriptor.settings)
       and persists it. Encoding: TOGGLE 1 byte (0 or 1); INT, SLIDER int32_t;
       CHOICE, COLOUR, KEYBIND uint32_t; CHANNEL uint64_t; TEXT, SECRET UTF-8;
       TEXT_LIST UTF-8 items separated by '\n'; USER_LIST uint64_t ids.
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

    /* -- Decorations, buttons and cache reads ---------------------------------- */
    /* Asks the decorate hook of `what` (MC_INVALIDATE_*) again for id at the next
       pass, showing the old verdict until then; id 0 = every entity of that kind.
       BUTTONS re-runs visible functions. */
    mc_status(MC_CALL* invalidate)(mc_ctx* ctx, uint32_t what, uint64_t id);
    /* Sets a decoration without a pull hook; it replaces a pulled one for that
       entity, which isn't asked again until a null or empty push clears it.
       kind: MC_HOOK_DECORATE_*; id: the message, user, channel or guild (HEADER:
       0 = both headers, or MC_HEADER_*); scope: MESSAGE the channel, else 0.
       The host keeps the most recent pushes per plugin (about 250 messages). */
    mc_status(MC_CALL* decorate_push)(mc_ctx* ctx, uint32_t kind, uint64_t id, uint64_t scope,
                                      const mc_decoration* decoration);
    /* button: index in descriptor.buttons; allowed in start. An all-zero state
       restores the declared look. */
    mc_status(MC_CALL* button_state)(mc_ctx* ctx, uint32_t button, const mc_button_state* state);
    /* Copies from MaiCord's cache; never fetches. MC_E_NOT_FOUND when not cached.
       Strings stay valid until the calling entry point returns. read_user:
       names come from guild when it isn't 0. */
    mc_status(MC_CALL* read_user)(mc_ctx* ctx, uint64_t user, uint64_t guild, mc_user* out);
    mc_status(MC_CALL* read_channel)(mc_ctx* ctx, uint64_t channel, mc_channel* out);
    mc_status(MC_CALL* read_guild)(mc_ctx* ctx, uint64_t guild, mc_guild* out);
    /* The user's permissions in channel (or guild-wide when channel is 0), as
       Discord permission bits. */
    mc_status(MC_CALL* read_permissions)(mc_ctx* ctx, uint64_t guild, uint64_t channel, uint64_t user,
                                         uint64_t* bits);

    /* -- Feedback and navigation ------------------------------------------------- */
    /* One banner per plugin; a new one replaces it. The result arrives as
       MC_COMPLETION_BANNER. */
    mc_status(MC_CALL* banner)(mc_ctx* ctx, const mc_banner* banner);
    /* MC_E_NOT_FOUND unless tag is the current banner's. */
    mc_status(MC_CALL* banner_clear)(mc_ctx* ctx, uint64_t tag);
    /* Gesture; MC_E_BUSY while any modal is open. The result arrives as
       MC_COMPLETION_MODAL, with a gesture when accepted. */
    mc_status(MC_CALL* modal)(mc_ctx* ctx, const mc_modal* modal, uint64_t tag);
    /* Gesture; http or https, at most MC_HTTP_URL_MAX; the host confirms external links. */
    mc_status(MC_CALL* open_url)(mc_ctx* ctx, mc_str url);
    /* Gesture and MC_PERM_CLIPBOARD; at most MC_ACTION_TEXT_MAX. */
    mc_status(MC_CALL* copy_text)(mc_ctx* ctx, mc_str text);
    /* Gesture; what: MC_OPEN_*. */
    mc_status(MC_CALL* open)(mc_ctx* ctx, uint32_t what, uint64_t id, uint64_t sub);

    /* -- Network ---------------------------------------------------------------- */
    /* Queues a request; the response arrives as MC_COMPLETION_HTTP with tag.
       MC_E_DENIED: the host isn't declared or approved, or is Discord's;
       MC_E_RATE_LIMITED past MC_HTTP_REQUESTS_PER_MINUTE; MC_E_BUSY past
       MC_HTTP_PENDING_MAX in flight. */
    mc_status(MC_CALL* http)(mc_ctx* ctx, const mc_http_request* request, uint64_t tag);

    /* -- Local messages and the composer ------------------------------------------- */
    /* A message only you see, in the open channel (channel 0, or its id; else MC_E_NOT_FOUND); cards
       attach with decorate_push(MC_HOOK_DECORATE_MESSAGE, *message_id, channel). */
    mc_status(MC_CALL* local_message)(mc_ctx* ctx, uint64_t channel, mc_str text, uint64_t* message_id);
    mc_status(MC_CALL* local_message_remove)(mc_ctx* ctx, uint64_t channel, uint64_t message);
    /* The composer calls need MC_PERM_COMPOSER. composer_text copies the
       open channel's draft like store_get. */
    mc_status(MC_CALL* composer_text)(mc_ctx* ctx, void* out, uint32_t capacity, uint32_t* bytes);
    mc_status(MC_CALL* composer_insert)(mc_ctx* ctx, mc_str text); /* at the caret */
    mc_status(MC_CALL* composer_replace)(mc_ctx* ctx, mc_str text);
    mc_status(MC_CALL* composer_focus)(mc_ctx* ctx);
    /* message 0 (with ping 0) clears the reply. */
    mc_status(MC_CALL* composer_reply)(mc_ctx* ctx, uint64_t channel, uint64_t message, uint32_t ping);
    mc_status(MC_CALL* composer_edit)(mc_ctx* ctx, uint64_t channel, uint64_t message);

    /* -- Actions (see Actions) ------------------------------------------------------ */
    mc_status(MC_CALL* send_message)(mc_ctx* ctx, const mc_send* send);
    mc_status(MC_CALL* edit_message)(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_str text);
    /* emoji: Unicode, or name:id for a custom emoji; flags: MC_REACT_REMOVE. */
    mc_status(MC_CALL* react)(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_str emoji, uint32_t flags);
    mc_status(MC_CALL* delete_own)(mc_ctx* ctx, uint64_t channel, uint64_t message);
    mc_status(MC_CALL* mark_read)(mc_ctx* ctx, uint32_t scope, uint64_t id); /* MC_READ_*; ALL: id 0 */
    /* MC_PERM_ACT_PRESENCE; no gesture needed, 2 per 10 s; null clears. */
    mc_status(MC_CALL* set_activity)(mc_ctx* ctx, const mc_activity* activity);

    /* -- Timers --------------------------------------------------------------------- */
    /* Calls MC_HOOK_TIMER with id after interval_ms (>= MC_TIMER_INTERVAL_MIN),
       once or repeatedly; the same id replaces a timer. MC_TIMERS_MAX per plugin. */
    mc_status(MC_CALL* timer)(mc_ctx* ctx, uint64_t id, uint32_t interval_ms, uint32_t repeat);
    mc_status(MC_CALL* cancel_timer)(mc_ctx* ctx, uint64_t id);

    /* -- Kept messages (MESSAGE_DELETED / MESSAGE_EDITED) ------------------------------ */
    mc_status(MC_CALL* keep_info)(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_kept_info* out);
    /* version 0: the latest content; 1..edit_count: older ones, 1 the most recent. */
    mc_status(MC_CALL* keep_text)(mc_ctx* ctx, uint64_t channel, uint64_t message, uint32_t version, void* out,
                                  uint32_t capacity, uint32_t* bytes);
    /* MC_PERM_PERSISTENT_LOG; at most MC_KEEP_RESTORE_MAX items per call. */
    mc_status(MC_CALL* keep_restore)(mc_ctx* ctx, const mc_kept* items, uint32_t count);

    /* -- Level 2 ---------------------------------------------------------------------- */
    /* The wall clock, Unix milliseconds (UTC). */
    mc_status(MC_CALL* clock)(mc_ctx* ctx, uint64_t* unix_ms);
    /* The user's time zone at unix_seconds: seconds east of UTC, daylight saving included. */
    mc_status(MC_CALL* local_offset)(mc_ctx* ctx, int64_t unix_seconds, int32_t* seconds_east);
    /* MC_PERM_READ_ACTIVITY. The voice channel user is in: in guild, or (guild 0) a private call first, then any server.
       MC_E_NOT_FOUND, with *channel 0, when in none MaiCord knows of. */
    mc_status(MC_CALL* read_voice_channel)(mc_ctx* ctx, uint64_t user, uint64_t guild, uint64_t* channel);
    /* MC_PERM_READ_MESSAGES. The group DMs you share with user, most recently active first: up to
       capacity ids into out; *count receives how many there are. MC_E_NOT_FOUND when none. */
    mc_status(MC_CALL* read_shared_group_dms)(mc_ctx* ctx, uint64_t user, uint64_t* out, uint32_t capacity,
                                              uint32_t* count);
    /* The open channel and the composer's reply and edit targets; MC_E_NOT_FOUND when no channel is open. */
    mc_status(MC_CALL* read_open_channel)(mc_ctx* ctx, mc_open_channel* out);
    /* MC_PERM_READ_MESSAGES. Messages of channel that MaiCord holds (the open channel's loaded history),
       newest first, starting below before (0: the newest); at most capacity (1..MC_READ_MESSAGES_MAX).
       *count receives how many were written; MC_E_NOT_FOUND when the channel isn't loaded. */
    mc_status(MC_CALL* read_messages)(mc_ctx* ctx, uint64_t channel, uint64_t before, mc_message_ref* out,
                                      uint32_t capacity, uint32_t* count);
};

/* True when the host's table contains `field` (for entries above the plugin's api_level). */
#define MC_API_HAS(ctx, field) ((ctx)->api->size >= offsetof(mc_api, field) + sizeof((ctx)->api->field))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAICORD_PLUGIN_H */

/*
 * MaiCord native plugin SDK, ABI 5. Pure C (C11 or C++), Windows x64 only.
 *
 * OVERVIEW
 *   A plugin is a DLL next to a "<name>.maicord-plugin.json" manifest in
 *   %LOCALAPPDATA%\MaiCord\plugins\<folder>. It exports
 *
 *     mc_status MC_CALL maicord_plugin_query_v5(const mc_host_info*, mc_plugin_definition*);
 *
 *   The query fills the definition (owner, permissions, state size, start/stop)
 *   and must not start work. The host then allocates zeroed, host-owned state
 *   and calls start(), passing an mc_host by pointer. Copy what you need: no
 *   pointer the host hands to plugin code survives the call that received it,
 *   except the immutable interface tables returned by get_interface.
 *   Every call happens on the UI thread. Do not create threads that call the host.
 *
 * MANIFEST (schema 3)
 *   { "schema": 3, "owner_id": 1234, "abi": 5, "min_host_abi": 5,
 *     "name": "Dice", "version": "1.0.0", "description": "...",
 *     "publisher": "Example", "permissions": ["commands", "store"],
 *     "dll": "dice.dll", "sha256": "<64 lowercase hex>" }
 *   Permission names are the MC_PERM_* names below in lower case without the
 *   prefix ("read_guilds", "act_messages", ...). The user sees and approves the
 *   list when enabling the plugin (Settings > Plugins shows it again on the
 *   plugin's row); the definition may request only approved permissions and
 *   every interface function checks the permission it needs.
 *
 * STRUCT GROWTH (size prefix)
 *   Every struct crossing the boundary starts with uint32_t size = sizeof as the
 *   writer compiled it. Readers use min(size, known) bytes and treat the rest as
 *   zero, so fields are only ever appended. Arrays carry an explicit stride.
 *   Output structs (mc_*_info) are filled up to the size the caller set; set
 *   size = sizeof before the call. Interface tables also start with size:
 *   check that a function pointer lies inside table->size before calling it
 *   (MC_HAS_FIELD).
 *
 * INTERFACES (mc_host.get_interface(binding, generation, name, version, &table))
 *   "mc.ui.pages"    1  settings pages in the Settings navigation   [settings]
 *   "mc.settings"    3  page content and keybind firing             [settings, keybinds]
 *   "mc.ui.menus"    2  items in user/message/channel/guild/
 *                       attachment/link/emoji context menus         [menus]
 *   "mc.msg.hooks"   2  pre-send transform                          [message_send]
 *   "mc.cmd"         1  local slash commands run by the composer    [commands]
 *   "mc.ui.slots"    2  declarative nodes the host lays out/paints  [ui_slots]
 *   "mc.store"       1  per-plugin bounded key-value storage        [store]
 *   "mc.read"        2  copies of client state                      [read_*]
 *   "mc.events"      1  batched change notifications                [read_*]
 *   "mc.msg.render"  1  message presentation decorators             [message_render]
 *   "mc.actions"     1  host-executed, rate-limited actions         [act_*, navigate, ...]
 *   "mc.timer"       1  coarse UI-thread timers                     [timer]
 *   "mc.fx"          1  background effect shaders                   [effects]
 *   A missing permission yields MC_E_DENIED; an unknown name or another version
 *   yields MC_E_UNSUPPORTED. Pages and commands register during start only;
 *   everything else may be called any time the plugin is enabled.
 *
 * SAFETY
 *   Every call into plugin code is timed. Each callback has a time budget
 *   (50 ms; query, start and stop 500 ms); a plugin that overruns it three
 *   times is turned off after the call returns and stays off until the user
 *   enables it again. A background effect whose GPU time stays above its
 *   budget is turned off the same way. The host records which plugin call is
 *   in progress; if MaiCord exits during a call, that plugin stays off at the
 *   next start and the user sees a notice. There is no in-process fault
 *   containment: plugins are trusted native code and must not rely on their
 *   own structured or C++ exceptions. This is crash attribution, not a sandbox.
 *   No interface exposes tokens, credentials, cookies or raw network access;
 *   actions go through MaiCord's own protocol code.
 *
 * USER GESTURES
 *   Handlers the user triggered directly (settings controls, menu items, slot
 *   and accessory buttons, popover rows, keybinds, commands and modal results)
 *   run inside a gesture. copy_text, open_url, open_modal, open_channel,
 *   open_profile and open_page are accepted only inside a gesture; elsewhere
 *   they return MC_E_INACTIVE.
 *
 * LIMITS (host may raise them; never rely on lower values)
 *   4 pages x 32 controls, 12 KiB of text and 101 slider steps per page;
 *   choices: 16 per control; 8 keybind controls per plugin; 16 menu items;
 *   8 commands x 4 options; 256 slot entries x 4 nodes; popovers: 8 nodes;
 *   store: 32 keys, 2 KiB values, 16 KiB total; 4 timers, interval >= 250 ms;
 *   pre-send output 4000 bytes; events: 32 per batch, 4 batches per plugin per
 *   pass, 512 queued; decorations: content <= 2000 bytes, 2 badges and 4
 *   accessory nodes per plugin; 2 effects per plugin, 64 KiB bytecode each;
 *   state <= 64 KiB, alignment <= 64.
 */
#ifndef MAICORD_PLUGIN_H
#define MAICORD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_CALL __stdcall
#define MC_EXPORT __declspec(dllexport)
#define MC_ABI_VERSION 5u
#define MC_QUERY_EXPORT "maicord_plugin_query_v5"
/* True when a size-prefixed struct or table written by the other side contains `field`. */
#define MC_HAS_FIELD(object, type, field) \
    ((object) && (object)->size >= offsetof(type, field) + sizeof(((type*)0)->field))

typedef struct mc_str {
    const char* data; /* UTF-8, not NUL-terminated */
    uint32_t bytes;
} mc_str;

typedef int32_t mc_status;
enum {
    MC_OK = 0,
    MC_E_INVALID = 1,     /* malformed argument or struct */
    MC_E_DENIED = 2,      /* permission not approved */
    MC_E_BUDGET = 3,      /* per-plugin limit reached */
    MC_E_NOT_FOUND = 4,   /* unknown id, or not cached (the host never fetches for a read) */
    MC_E_INACTIVE = 5,    /* stale binding/generation or not allowed now (e.g. outside a gesture) */
    MC_E_UNSUPPORTED = 6, /* unknown interface or version */
    MC_E_CAPACITY = 7,    /* value larger than the host accepts */
    MC_E_TRUNCATED = 8,   /* output did not fit; the needed size is reported */
    MC_E_BUSY = 9,        /* the host can't take this now (another request in progress) */
    MC_E_RATE_LIMITED = 10 /* the action's rate limit is exhausted; retry later */
};

/* Permissions (bit mask, manifest names in brackets). */
#define MC_PERM_SETTINGS (1ull << 0)       /* [settings] settings pages */
#define MC_PERM_MENUS (1ull << 1)          /* [menus] context menu items */
#define MC_PERM_MESSAGE_SEND (1ull << 2)   /* [message_send] change your messages before sending */
#define MC_PERM_COMMANDS (1ull << 3)       /* [commands] local slash commands */
#define MC_PERM_UI_SLOTS (1ull << 4)       /* [ui_slots] badges and buttons */
#define MC_PERM_STORE (1ull << 5)          /* [store] own data storage */
#define MC_PERM_TIMER (1ull << 6)          /* [timer] timers */
#define MC_PERM_READ_CONTEXT (1ull << 7)   /* [read_context] open channel, your account */
#define MC_PERM_READ_GUILDS (1ull << 8)    /* [read_guilds] servers, channels, roles */
#define MC_PERM_READ_MEMBERS (1ull << 9)   /* [read_members] users, members, friends, profiles */
#define MC_PERM_READ_PRESENCE (1ull << 10) /* [read_presence] status, devices, activities */
#define MC_PERM_READ_VOICE (1ull << 11)    /* [read_voice] who is in voice */
#define MC_PERM_READ_MESSAGES (1ull << 12) /* [read_messages] messages you receive */
#define MC_PERM_MESSAGE_RENDER (1ull << 13) /* [message_render] change how messages look */
#define MC_PERM_ACT_MESSAGES (1ull << 14)  /* [act_messages] send, edit, react, mark read */
#define MC_PERM_ACT_PRESENCE (1ull << 15)  /* [act_presence] custom status and activity */
#define MC_PERM_NAVIGATE (1ull << 16)      /* [navigate] open channels, profiles, own pages */
#define MC_PERM_CLIPBOARD (1ull << 17)     /* [clipboard] copy text */
#define MC_PERM_OPEN_LINKS (1ull << 18)    /* [open_links] open links (with confirmation) */
#define MC_PERM_NOTIFY (1ull << 19)        /* [notify] notices and dialogs */
#define MC_PERM_KEYBINDS (1ull << 20)      /* [keybinds] keyboard shortcuts */
#define MC_PERM_EFFECTS (1ull << 21)       /* [effects] background effects */
#define MC_PERM_ALL ((1ull << 22) - 1)

/* Tones map to theme roles; plugins never choose colours or draw. */
enum {
    MC_TONE_NEUTRAL = 0,
    MC_TONE_ACCENT = 1,
    MC_TONE_POSITIVE = 2,
    MC_TONE_WARNING = 3,
    MC_TONE_CRITICAL = 4,
    MC_TONE_ROLE = 5 /* decorations only: the author's role colour (name suffix/prefix) */
};

/* ---- Lifecycle ---------------------------------------------------------- */

typedef struct mc_host {
    uint32_t size;
    uint32_t abi_version;
    void* binding;       /* opaque; pass back with generation on every call */
    uint64_t generation; /* changes on every enable; stale copies are refused */
    mc_status (MC_CALL* get_interface)(void* binding, uint64_t generation, const char* name,
                                       uint32_t version, const void** table);
} mc_host;

typedef struct mc_host_info {
    uint32_t size;
    uint32_t abi_version; /* host ABI (>= 5) */
} mc_host_info;

typedef struct mc_plugin_definition {
    uint32_t size;
    uint32_t abi_version; /* MC_ABI_VERSION */
    uint64_t owner_id;    /* must equal the manifest owner_id */
    uint64_t permissions; /* MC_PERM_* requested; subset of the approved set */
    uint32_t state_bytes; /* host-owned zeroed state passed to callbacks */
    uint32_t state_alignment;
    uint32_t min_host_abi;
    uint32_t reserved;
    /* Return MC_OK to stay enabled. stop always follows an attempted start
       unless the plugin faulted; it must accept partially initialized state. */
    mc_status (MC_CALL* start)(const mc_host* host, void* state, uint32_t state_bytes);
    void (MC_CALL* stop)(void* state, uint32_t state_bytes);
} mc_plugin_definition;

typedef mc_status (MC_CALL* mc_query_fn)(const mc_host_info* host, mc_plugin_definition* definition);

/* Copies the plugin's definition into the host buffer by the prefix rule. */
static __inline mc_status mc_definition_publish(mc_plugin_definition* out, const mc_plugin_definition* local) {
    uint32_t bytes, i;
    if (!out || !local || out->size < offsetof(mc_plugin_definition, start)) return MC_E_INVALID;
    bytes = out->size < local->size ? out->size : local->size;
    for (i = 0; i < bytes; ++i) ((unsigned char*)out)[i] = ((const unsigned char*)local)[i];
    out->size = bytes;
    return MC_OK;
}

/* ---- Declarative nodes (slots, decorations, popovers, modals) ----------- */

enum {
    MC_NODE_LABEL = 0,     /* text */
    MC_NODE_ICON = 1,      /* icon (tooltip as its accessible name) */
    MC_NODE_BADGE = 2,     /* compact pill: text and/or icon */
    MC_NODE_BUTTON = 3,    /* text and/or icon, action_id != 0 */
    MC_NODE_LINK = 4,      /* text opening `value` (https URL) through the link confirmation */
    MC_NODE_SEPARATOR = 5, /* popovers and modals: a divider */
    MC_NODE_INPUT = 6      /* modals only: single-line field; text = label, value = initial text */
};

typedef struct mc_slot_node {
    uint32_t size;
    uint32_t kind;  /* MC_NODE_* */
    mc_str text;    /* badges/buttons <= 32 bytes; labels and links <= 256 bytes */
    mc_str icon;    /* icon pack name, e.g. "lucide/star"; unknown names show the plugin glyph */
    mc_str tooltip; /* <= 64 bytes */
    uint32_t tone;  /* MC_TONE_* */
    uint32_t reserved;
    uint64_t action_id; /* buttons: non-zero, reported back to the handler */
    mc_str value;       /* links: https URL <= 512 bytes; inputs: initial text */
} mc_slot_node;

/* ---- mc.ui.pages@1 ------------------------------------------------------ */

typedef struct mc_pages_v1 {
    uint32_t size;
    uint32_t version;
    /* The label is copied. Content comes from mc.settings set_page. */
    mc_status (MC_CALL* register_page)(void* binding, uint64_t generation, uint64_t page_id, mc_str label);
    mc_status (MC_CALL* unregister_page)(void* binding, uint64_t generation, uint64_t page_id);
} mc_pages_v1;

/* ---- mc.settings@3 ------------------------------------------------------ */

enum {
    MC_SETTING_TOGGLE = 0,
    MC_SETTING_CHOICE = 1,
    MC_SETTING_ACTION = 2,
    MC_SETTING_TEXT = 3,    /* single line, max_bytes */
    MC_SETTING_SLIDER = 4,  /* minimum..maximum by step, at most 101 steps */
    MC_SETTING_COLOUR = 5,  /* 0xRRGGBB */
    MC_SETTING_KEYBIND = 6, /* "Ctrl+Shift+K" style chord, normalized by the host; fires the keybind handler */
    MC_SETTING_HEADER = 7,  /* section heading */
    MC_SETTING_NOTICE = 8   /* status text with an optional action button */
};

typedef struct mc_setting {
    uint32_t size;
    uint32_t kind;
    uint64_t id; /* non-zero, unique on the page */
    mc_str label;
    mc_str description;
    uint32_t enabled;
    uint32_t checked; /* toggle */
    const mc_str* choices; /* choice */
    uint32_t choice_count;
    uint32_t selected;
    mc_str action_label; /* action, notice */
    mc_str text;         /* text, keybind */
    uint32_t max_bytes;  /* text; 0 means 256 */
    int32_t minimum, maximum, step, value; /* slider */
    uint32_t colour;     /* colour */
} mc_setting;

typedef struct mc_settings_page {
    uint32_t size;
    uint32_t control_count;
    mc_str title;
    mc_str description;
    const mc_setting* controls;
    uint32_t control_stride; /* sizeof(mc_setting) as compiled by the plugin */
    uint32_t reserved;
} mc_settings_page;

/* Host -> plugin when the user changes a control. text is valid during the call. */
typedef struct mc_setting_value {
    uint32_t size;
    uint32_t kind;
    uint64_t page_id;
    uint64_t control_id;
    uint32_t checked;
    uint32_t selected;
    int32_t value;
    uint32_t colour;
    mc_str text;
} mc_setting_value;

/* Republish with set_page (allowed inside the handler) when values change. */
typedef mc_status (MC_CALL* mc_setting_fn)(void* state, uint32_t state_bytes, const mc_setting_value* value);
/* A published, enabled keybind control's chord was pressed (a gesture). */
typedef void (MC_CALL* mc_keybind_fn)(void* state, uint32_t state_bytes, uint64_t page_id, uint64_t control_id);

/*
 * Keybinds: every enabled MC_SETTING_KEYBIND control with a chord is
 * registered with MaiCord's shortcut registry when its page is published,
 * provided the plugin holds [keybinds] and set a keybind handler. A chord
 * needs Ctrl or Alt (or is F1-F24), may not use Win, and is refused when it
 * matches a built-in shortcut, a system/editing chord (Ctrl+C, Alt+F4, ...),
 * a custom keybind or another plugin's chord; the control's row then says
 * what it conflicts with. Plugin chords fire while a text field or the
 * composer has focus only when they include Ctrl or Alt, never while a
 * dialog or menu is open, and never when the window is in the background.
 */
typedef struct mc_settings_v3 {
    uint32_t size;
    uint32_t version;
    /* Copies the whole page; the page must be registered by mc.ui.pages. */
    mc_status (MC_CALL* set_page)(void* binding, uint64_t generation, uint64_t page_id, const mc_settings_page* page);
    mc_status (MC_CALL* set_handler)(void* binding, uint64_t generation, mc_setting_fn handler);
    /* [keybinds] */
    mc_status (MC_CALL* set_keybind_handler)(void* binding, uint64_t generation, mc_keybind_fn handler);
} mc_settings_v3;

/* ---- mc.ui.menus@2 ------------------------------------------------------ */

enum {
    MC_MENU_USER = 0,       /* any user: member list, author, DM row, voice row, profile, friends */
    MC_MENU_MESSAGE = 1,    /* chat message */
    MC_MENU_CHANNEL = 2,    /* channel, thread, category or DM row */
    MC_MENU_GUILD = 3,      /* server rail icon or server header */
    MC_MENU_ATTACHMENT = 4, /* attachment, image, video or file */
    MC_MENU_LINK = 5,       /* link in a message */
    MC_MENU_EMOJI = 6,      /* reaction or emoji */
    MC_MENU_TARGET_COUNT = 7
};
#define MC_MENU_ITEM_DANGER 1u   /* critical tone */
#define MC_MENU_ITEM_DISABLED 2u

/* Typed context; ids that don't apply are zero. Strings are valid during the call. */
#define MC_MENU_OWN 1u      /* own message or own user */
#define MC_MENU_PRIVATE 2u  /* the channel is a DM or group DM */
#define MC_MENU_ANIMATED 4u /* animated emoji */
typedef struct mc_menu_context {
    uint32_t size;
    uint32_t target; /* MC_MENU_* */
    uint64_t item_id;
    uint64_t user_id;    /* user target, message author, DM recipient */
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t guild_id;
    uint64_t emoji_id;   /* custom emoji; zero for Unicode */
    uint32_t attachment_index;
    uint32_t flags;      /* MC_MENU_OWN, MC_MENU_PRIVATE, MC_MENU_ANIMATED */
    mc_str url;          /* link and attachment URL */
    mc_str text;         /* link text, attachment filename, emoji name or Unicode emoji */
} mc_menu_context;

/* Runs inside a gesture: actions such as copy_text are allowed. */
typedef mc_status (MC_CALL* mc_menu_fn)(void* state, uint32_t state_bytes, const mc_menu_context* context);

typedef struct mc_menus_v2 {
    uint32_t size;
    uint32_t version;
    /* label <= 64 bytes, copied; item_id non-zero and unique per plugin. */
    mc_status (MC_CALL* register_item)(void* binding, uint64_t generation, uint64_t item_id, uint32_t target,
                                       mc_str label, uint32_t flags);
    mc_status (MC_CALL* unregister_item)(void* binding, uint64_t generation, uint64_t item_id);
    mc_status (MC_CALL* set_handler)(void* binding, uint64_t generation, mc_menu_fn handler);
} mc_menus_v2;

/* ---- mc.msg.hooks@2 ----------------------------------------------------- */

#define MC_MESSAGE_OWN 1u
#define MC_MESSAGE_BOT 2u
#define MC_MESSAGE_EDITED 4u
#define MC_MESSAGE_PINNED 8u
#define MC_MESSAGE_REPLY 16u
#define MC_MESSAGE_MEDIA 32u /* has attachments or embeds */

typedef struct mc_message_view {
    uint32_t size;
    uint32_t flags; /* MC_MESSAGE_* */
    uint64_t channel_id;
    uint64_t message_id; /* zero for drafts */
    uint64_t author_id;
    uint64_t guild_id;
    uint64_t timestamp_ms;
    mc_str content;
    mc_str author_name;
} mc_message_view;

enum { MC_SEND_UNCHANGED = 0, MC_SEND_REPLACE = 1, MC_SEND_CANCEL = 2 };

/* Host-owned output buffer; write at most capacity bytes of UTF-8. */
typedef struct mc_text_output {
    uint32_t size;
    uint32_t action;   /* MC_SEND_* or MC_CMD_* */
    char* data;
    uint32_t capacity;
    uint32_t bytes;
} mc_text_output;

typedef mc_status (MC_CALL* mc_pre_send_fn)(void* state, uint32_t state_bytes, const mc_message_view* draft,
                                            mc_text_output* output);

typedef struct mc_msg_hooks_v2 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* set_pre_send)(void* binding, uint64_t generation, mc_pre_send_fn handler);
} mc_msg_hooks_v2;

/* ---- mc.cmd@1 ----------------------------------------------------------- */

enum { MC_OPTION_STRING = 0, MC_OPTION_INTEGER = 1, MC_OPTION_BOOLEAN = 2 };

typedef struct mc_command_option {
    uint32_t size;
    uint32_t type;
    mc_str name;
    mc_str description;
    uint32_t required;
    uint32_t reserved;
} mc_command_option;

typedef struct mc_command {
    uint32_t size;
    uint32_t option_count;
    uint64_t command_id;
    mc_str name; /* lowercase a-z 0-9 - _, 1..32 bytes, typed as /name */
    mc_str description;
    const mc_command_option* options; /* positional; a last string option takes the rest */
    uint32_t option_stride;
    uint32_t reserved;
} mc_command;

typedef struct mc_command_argument {
    uint32_t size;
    uint32_t type;
    uint32_t present;
    uint32_t boolean;
    int64_t integer;
    mc_str name;
    mc_str text;
} mc_command_argument;

typedef struct mc_command_invocation {
    uint32_t size;
    uint32_t argument_count;
    uint64_t command_id;
    uint64_t channel_id;
    uint64_t guild_id;
    const mc_command_argument* arguments;
    uint32_t argument_stride;
    uint32_t reserved;
    mc_str raw; /* text after the command name */
} mc_command_invocation;

/* Command result actions in mc_text_output.action. */
enum { MC_CMD_DONE = 0, MC_CMD_SEND = 1, MC_CMD_REPLACE = 2, MC_CMD_NOTICE = 3 };

/* Runs inside a gesture. */
typedef mc_status (MC_CALL* mc_command_fn)(void* state, uint32_t state_bytes, const mc_command_invocation* call,
                                           mc_text_output* result);

typedef struct mc_cmd_v1 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* register_command)(void* binding, uint64_t generation, const mc_command* command);
    mc_status (MC_CALL* unregister_command)(void* binding, uint64_t generation, uint64_t command_id);
    mc_status (MC_CALL* set_handler)(void* binding, uint64_t generation, mc_command_fn handler);
} mc_cmd_v1;

/* ---- mc.ui.slots@2 ------------------------------------------------------ */

enum {
    MC_SLOT_MESSAGE_ACCESSORY = 0, /* badge after the author name; target: channel_id + message_id */
    MC_SLOT_COMPOSER_TOOLBAR = 1,  /* square buttons by the composer; global */
    MC_SLOT_MEMBER_BADGE = 2,      /* badge on member list rows; target: user_id */
    MC_SLOT_PROFILE_SECTION = 3,   /* rows in profiles and popouts; target: user_id */
    MC_SLOT_HEADER_TOOLBAR = 4,    /* badge in the channel header; global */
    MC_SLOT_CHANNEL_ROW = 5,       /* badge on channel and DM rows; target: channel_id */
    MC_SLOT_GUILD_RAIL = 6,        /* badge on a server icon; target: guild_id */
    MC_SLOT_ACCOUNT_PANEL = 7,     /* square buttons in the account panel; global */
    MC_SLOT_MESSAGE_HOVER = 8,     /* buttons in the message hover toolbar; global, clicks carry the message */
    MC_SLOT_SURFACE_COUNT = 9
};

typedef struct mc_slot_target {
    uint32_t size;
    uint32_t surface;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t user_id;
    uint64_t guild_id;
} mc_slot_target;

typedef struct mc_slot_content {
    uint32_t size;
    uint32_t node_count; /* <= 4 */
    const mc_slot_node* nodes;
    uint32_t node_stride;
    uint32_t reserved;
} mc_slot_content;

/* What was clicked: the surface's target ids (for MESSAGE_HOVER the hovered
   message; for popover rows the popover's anchor) and the node's action_id. */
typedef struct mc_slot_event {
    uint32_t size;
    uint32_t surface;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t user_id;
    uint64_t guild_id;
    uint64_t action_id;
} mc_slot_event;

/* Optional handler output: up to 8 nodes the host shows as a popover anchored
   to the clicked control (buttons run the handler again with their action_id,
   links open through the link confirmation, labels and separators are inert). */
typedef struct mc_popover {
    uint32_t size;
    uint32_t node_count;
    const mc_slot_node* nodes;
    uint32_t node_stride;
    uint32_t reserved;
} mc_popover;

/* Runs inside a gesture. popover is copied after the call returns. */
typedef mc_status (MC_CALL* mc_slot_action_fn)(void* state, uint32_t state_bytes, const mc_slot_event* event,
                                               mc_popover* popover);

typedef struct mc_slots_v2 {
    uint32_t size;
    uint32_t version;
    /* Replaces this plugin's nodes for the target. Oldest entries are evicted.
       Global surfaces ignore target ids; zero nodes clears. */
    mc_status (MC_CALL* set)(void* binding, uint64_t generation, const mc_slot_target* target,
                             const mc_slot_content* content);
    mc_status (MC_CALL* clear)(void* binding, uint64_t generation, const mc_slot_target* target);
    /* Removes every entry of one surface (MC_SLOT_SURFACE_COUNT: all surfaces). */
    mc_status (MC_CALL* clear_surface)(void* binding, uint64_t generation, uint32_t surface);
    mc_status (MC_CALL* set_handler)(void* binding, uint64_t generation, mc_slot_action_fn handler);
} mc_slots_v2;

/* ---- mc.store@1 --------------------------------------------------------- */

typedef struct mc_store_v1 {
    uint32_t size;
    uint32_t version;
    /* Keys: 1..64 bytes of printable ASCII. */
    mc_status (MC_CALL* get)(void* binding, uint64_t generation, mc_str key, void* buffer, uint32_t capacity,
                             uint32_t* bytes);
    mc_status (MC_CALL* set)(void* binding, uint64_t generation, mc_str key, const void* value, uint32_t bytes);
    mc_status (MC_CALL* remove)(void* binding, uint64_t generation, mc_str key);
    mc_status (MC_CALL* usage)(void* binding, uint64_t generation, uint32_t* used_bytes, uint32_t* limit_bytes);
} mc_store_v1;

/* ---- mc.read@2 ---------------------------------------------------------- */
/*
 * Copy-out getters. Each fills the caller's struct up to its size and never
 * fetches: data MaiCord doesn't hold yields MC_E_NOT_FOUND. Text is truncated
 * on a UTF-8 boundary to the inline arrays; *_bytes reports what was copied.
 * Ids are snowflakes; times are Unix milliseconds; zero means unknown.
 */

enum { MC_STATUS_UNKNOWN = 0, MC_STATUS_ONLINE = 1, MC_STATUS_IDLE = 2, MC_STATUS_DND = 3, MC_STATUS_OFFLINE = 4 };
enum {
    MC_RELATIONSHIP_NONE = 0,
    MC_RELATIONSHIP_FRIEND = 1,
    MC_RELATIONSHIP_BLOCKED = 2,
    MC_RELATIONSHIP_INCOMING = 3,
    MC_RELATIONSHIP_OUTGOING = 4,
    MC_RELATIONSHIP_IMPLICIT = 5
};

/* [read_context] */
typedef struct mc_context_info {
    uint32_t size;
    uint32_t flags;      /* reserved */
    uint64_t channel_id; /* zero when no channel is open */
    uint64_t guild_id;
    uint64_t user_id;    /* your account */
    uint32_t channel_name_bytes;
    uint32_t user_name_bytes;
    char channel_name[100];
    char user_name[64];
} mc_context_info;

/* [read_guilds] */
#define MC_GUILD_UNAVAILABLE 1u
#define MC_GUILD_OWNER 2u        /* you own it */
#define MC_GUILD_COMMUNITY 4u
#define MC_GUILD_COUNTS 8u       /* member_count/online_count are known */
typedef struct mc_guild_info {
    uint32_t size;
    uint32_t flags;
    uint64_t guild_id;
    uint64_t owner_id;
    uint64_t created_ms;
    uint64_t self_permissions; /* base permission bits for your account */
    uint32_t member_count;
    uint32_t online_count;
    uint32_t name_bytes;
    uint32_t icon_bytes;
    char name[100];
    char icon_hash[40];
} mc_guild_info;

#define MC_CHANNEL_NSFW 1u
#define MC_CHANNEL_PERMISSIONS 2u /* self_permissions is resolved */
#define MC_CHANNEL_THREAD 4u
#define MC_CHANNEL_PRIVATE 8u     /* DM or group DM */
#define MC_CHANNEL_CAN_SEND 16u
typedef struct mc_channel_info {
    uint32_t size;
    uint32_t type; /* channel type (wire value) */
    uint64_t channel_id;
    uint64_t guild_id;
    uint64_t parent_id;
    uint64_t last_message_id;
    uint64_t self_permissions; /* computed permission bits (MC_CHANNEL_PERMISSIONS) */
    uint64_t recipient_id;     /* DMs: the other user */
    uint32_t flags;
    uint32_t position;
    uint32_t name_bytes;
    uint32_t reserved;
    char name[100];
} mc_channel_info;

#define MC_ROLE_HOIST 1u
#define MC_ROLE_MANAGED 2u
#define MC_ROLE_MENTIONABLE 4u
typedef struct mc_role_info {
    uint32_t size;
    uint32_t flags;
    uint64_t role_id;
    uint64_t guild_id;
    uint64_t permissions;
    uint32_t colour;   /* 0xRRGGBB as data (plugins never paint); zero for none */
    uint32_t position;
    uint32_t name_bytes;
    uint32_t reserved;
    char name[100];
} mc_role_info;

/* [read_members] */
#define MC_MEMBER_ROLES 1u     /* role_ids are known */
#define MC_MEMBER_TRUNCATED 2u /* more than 16 roles */
#define MC_MEMBER_TIMED_OUT 4u
typedef struct mc_member_info {
    uint32_t size;
    uint32_t flags;
    uint64_t guild_id;
    uint64_t user_id;
    uint64_t timeout_until_ms;
    uint64_t colour_role_id;
    uint32_t status;     /* MC_STATUS_* as seen in this server's member list */
    uint32_t role_count;
    uint64_t role_ids[16];
    uint32_t nick_bytes;
    uint32_t reserved;
    char nick[96];
} mc_member_info;

#define MC_USER_SELF 1u
typedef struct mc_user_info {
    uint32_t size;
    uint32_t flags;
    uint64_t user_id;
    uint32_t name_bytes;     /* display (global) name, or username */
    uint32_t username_bytes;
    uint32_t avatar_bytes;
    uint32_t reserved;
    char name[64];
    char username[40];
    char avatar_hash[40];
} mc_user_info;

typedef struct mc_relationship_info {
    uint32_t size;
    uint32_t type;   /* MC_RELATIONSHIP_* */
    uint64_t user_id;
    uint32_t status; /* friends: MC_STATUS_* */
    uint32_t nickname_bytes;
    char nickname[64];
} mc_relationship_info;

/* Held only while that profile is open in MaiCord. */
typedef struct mc_profile_info {
    uint32_t size;
    uint32_t flags; /* reserved */
    uint64_t user_id;
    uint64_t guild_id;  /* server profile shown, or zero */
    uint64_t joined_ms; /* server join time when guild_id is set */
    uint32_t pronouns_bytes;
    uint32_t bio_bytes;
    char pronouns[64];
    char bio[800];
} mc_profile_info;

/* [read_presence] */
typedef struct mc_presence_info {
    uint32_t size;
    uint32_t status; /* MC_STATUS_* */
    uint64_t user_id;
    uint32_t desktop; /* per platform (client_status): MC_STATUS_*; MC_STATUS_OFFLINE when absent */
    uint32_t mobile;
    uint32_t web;
    uint32_t activity_count;
} mc_presence_info;

enum {
    MC_ACTIVITY_PLAYING = 0,
    MC_ACTIVITY_STREAMING = 1,
    MC_ACTIVITY_LISTENING = 2,
    MC_ACTIVITY_WATCHING = 3,
    MC_ACTIVITY_CUSTOM = 4,
    MC_ACTIVITY_COMPETING = 5
};
typedef struct mc_activity_info {
    uint32_t size;
    uint32_t type; /* MC_ACTIVITY_* */
    uint64_t application_id;
    uint64_t start_ms;
    uint64_t end_ms;
    uint32_t name_bytes;
    uint32_t details_bytes;
    uint32_t state_bytes;
    uint32_t reserved;
    char name[128];
    char details[128];
    char state[128];
} mc_activity_info;

/* [read_voice] */
#define MC_VOICE_MUTE 1u
#define MC_VOICE_DEAF 2u
#define MC_VOICE_SELF_MUTE 4u
#define MC_VOICE_SELF_DEAF 8u
#define MC_VOICE_STREAMING 16u
#define MC_VOICE_VIDEO 32u
#define MC_VOICE_SUPPRESSED 64u
typedef struct mc_voice_info {
    uint32_t size;
    uint32_t flags; /* MC_VOICE_* */
    uint64_t guild_id;
    uint64_t channel_id;
    uint64_t user_id;
} mc_voice_info;

/* [read_messages] Messages in MaiCord's cache (the open channel's window).
   Set content/content_capacity to receive the text; content_bytes reports
   the full size, and MC_E_TRUNCATED means the text was cut (other fields are
   complete). */
typedef struct mc_message_info {
    uint32_t size;
    uint32_t content_capacity; /* in */
    char* content;             /* in: caller buffer, or null */
    uint32_t content_bytes;
    uint32_t flags;            /* MC_MESSAGE_* */
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t author_id;
    uint64_t guild_id;
    uint64_t timestamp_ms;
    uint64_t edited_ms;
    uint64_t reply_message_id;
    uint64_t reply_channel_id;
    uint32_t type;             /* message type (wire value) */
    uint32_t revision;         /* changes whenever MaiCord updates the message */
    uint32_t attachment_count;
    uint32_t embed_count;
    uint32_t reaction_count;
    uint32_t author_name_bytes;
    char author_name[64];
} mc_message_info;

#define MC_ATTACHMENT_SPOILER 1u
typedef struct mc_attachment_info {
    uint32_t size;
    uint32_t flags;
    uint64_t attachment_id;
    uint64_t bytes;
    uint32_t width;
    uint32_t height;
    uint32_t filename_bytes;
    uint32_t content_type_bytes;
    uint32_t url_bytes;
    uint32_t reserved;
    char filename[128];
    char content_type[64];
    char url[512];
} mc_attachment_info;

typedef struct mc_embed_info {
    uint32_t size;
    uint32_t reserved;
    uint64_t timestamp_ms;
    uint32_t title_bytes;
    uint32_t url_bytes;
    uint32_t description_bytes;
    uint32_t provider_bytes;
    char title[256];
    char url[512];
    char description[512];
    char provider[64];
} mc_embed_info;

#define MC_REACTION_ME 1u
#define MC_REACTION_ANIMATED 2u
typedef struct mc_reaction_info {
    uint32_t size;
    uint32_t flags;
    uint64_t emoji_id; /* zero for Unicode */
    uint32_t count;
    uint32_t name_bytes;
    char name[64]; /* custom emoji name or the Unicode emoji */
} mc_reaction_info;

/* Monotonic change counters per data class (cheap polling). */
typedef struct mc_read_revisions {
    uint32_t size;
    uint32_t reserved;
    uint64_t guilds;   /* guilds, channels, roles */
    uint64_t members;  /* users, members, relationships */
    uint64_t presence;
    uint64_t voice;
    uint64_t messages;
} mc_read_revisions;

enum {
    MC_LIST_GUILDS = 0,        /* scope 0; [read_guilds] */
    MC_LIST_CHANNELS = 1,      /* scope guild, or 0 for DMs; [read_guilds] */
    MC_LIST_ROLES = 2,         /* scope guild; [read_guilds] */
    MC_LIST_MEMBERS = 3,       /* scope guild: members MaiCord holds; [read_members] */
    MC_LIST_RELATIONSHIPS = 4, /* scope 0: user ids; [read_members] */
    MC_LIST_VOICE = 5,         /* scope channel: user ids in it; [read_voice] */
    MC_LIST_MESSAGES = 6       /* scope channel: cached message ids, newest first; [read_messages] */
};

typedef struct mc_read_v2 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* context)(void* binding, uint64_t generation, mc_context_info* info);
    mc_status (MC_CALL* guild)(void* binding, uint64_t generation, uint64_t guild_id, mc_guild_info* info);
    mc_status (MC_CALL* channel)(void* binding, uint64_t generation, uint64_t channel_id, mc_channel_info* info);
    mc_status (MC_CALL* role)(void* binding, uint64_t generation, uint64_t guild_id, uint64_t role_id,
                              mc_role_info* info);
    mc_status (MC_CALL* member)(void* binding, uint64_t generation, uint64_t guild_id, uint64_t user_id,
                                mc_member_info* info);
    /* Your own user is readable with [read_context] alone. */
    mc_status (MC_CALL* user)(void* binding, uint64_t generation, uint64_t user_id, mc_user_info* info);
    mc_status (MC_CALL* relationship)(void* binding, uint64_t generation, uint64_t user_id,
                                      mc_relationship_info* info);
    mc_status (MC_CALL* profile)(void* binding, uint64_t generation, uint64_t user_id, mc_profile_info* info);
    /* guild_id selects which member list's status to prefer; zero uses friends first. */
    mc_status (MC_CALL* presence)(void* binding, uint64_t generation, uint64_t user_id, uint64_t guild_id,
                                  mc_presence_info* info);
    mc_status (MC_CALL* activity)(void* binding, uint64_t generation, uint64_t user_id, uint32_t index,
                                  mc_activity_info* info);
    /* guild_id zero searches every server and private calls. */
    mc_status (MC_CALL* voice)(void* binding, uint64_t generation, uint64_t guild_id, uint64_t user_id,
                               mc_voice_info* info);
    mc_status (MC_CALL* message)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                                 mc_message_info* info);
    mc_status (MC_CALL* attachment)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                                    uint32_t index, mc_attachment_info* info);
    mc_status (MC_CALL* embed)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                               uint32_t index, mc_embed_info* info);
    mc_status (MC_CALL* reaction)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                                  uint32_t index, mc_reaction_info* info);
    /* Bounded iteration: copies up to capacity ids starting at cursor (0 first);
       *next is the cursor to continue with, 0 at the end. */
    mc_status (MC_CALL* list)(void* binding, uint64_t generation, uint32_t list, uint64_t scope, uint32_t cursor,
                              uint64_t* ids, uint32_t capacity, uint32_t* count, uint32_t* next);
    /* Fills the counters of the classes this plugin may read; others stay zero. */
    mc_status (MC_CALL* revisions)(void* binding, uint64_t generation, mc_read_revisions* revisions);
} mc_read_v2;

/* ---- mc.events@1 -------------------------------------------------------- */
/*
 * Change notifications, copied out of the gateway stream before MaiCord
 * applies them and delivered in batches on the UI thread during the next
 * host pass (32 events per call, 4 calls per plugin per pass, oldest first).
 * The queue holds 512 events across plugins; a plugin that falls behind gets
 * one MC_EVENT_OVERFLOW with value = events lost. Each kind needs its read
 * permission: messages, typing and reactions [read_messages], presence
 * [read_presence], voice [read_voice], channels and guilds [read_guilds],
 * members [read_members]. Events cover every channel MaiCord receives.
 */
enum {
    MC_EVENT_MESSAGE_CREATE = 0, /* text: content, name: author */
    MC_EVENT_MESSAGE_UPDATE = 1, /* text: new content when the update carries it (MC_EVENT_TEXT) */
    MC_EVENT_MESSAGE_DELETE = 2, /* text/name/user_id: last cached copy (MC_EVENT_CACHED); MC_EVENT_BULK */
    MC_EVENT_TYPING = 3,         /* user_id typing in channel_id */
    MC_EVENT_REACTION_ADD = 4,   /* value: emoji id, text: emoji name, user_id: reactor */
    MC_EVENT_REACTION_REMOVE = 5,
    MC_EVENT_PRESENCE = 6,       /* value: MC_STATUS_* */
    MC_EVENT_VOICE_STATE = 7,    /* channel_id: joined channel (0 left), value: MC_VOICE_* */
    MC_EVENT_CHANNEL_CREATE = 8, /* value: channel type */
    MC_EVENT_CHANNEL_UPDATE = 9,
    MC_EVENT_CHANNEL_DELETE = 10,
    MC_EVENT_GUILD_CREATE = 11,
    MC_EVENT_GUILD_UPDATE = 12,
    MC_EVENT_GUILD_DELETE = 13,
    MC_EVENT_MEMBER_ADD = 14,
    MC_EVENT_MEMBER_UPDATE = 15,
    MC_EVENT_MEMBER_REMOVE = 16,
    MC_EVENT_KIND_COUNT = 17,
    MC_EVENT_OVERFLOW = 63       /* always delivered; value: events lost */
};
#define MC_EVENT_MASK(kind) (1ull << (kind))
#define MC_EVENT_OWN 1u    /* your own message, reaction, presence or voice state */
#define MC_EVENT_BOT 2u
#define MC_EVENT_BULK 4u   /* part of a bulk delete */
#define MC_EVENT_CACHED 8u /* text/name come from MaiCord's cache */
#define MC_EVENT_TEXT 16u  /* text is present */

typedef struct mc_event {
    uint32_t size;
    uint32_t kind; /* MC_EVENT_* */
    uint64_t sequence; /* host order */
    uint64_t guild_id;
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t user_id;
    uint64_t value;   /* per kind, see above */
    uint64_t time_ms; /* when MaiCord received it */
    uint32_t flags;   /* MC_EVENT_* flags */
    uint32_t reserved;
    mc_str text;      /* valid during the call */
    mc_str name;
} mc_event;

typedef void (MC_CALL* mc_event_fn)(void* state, uint32_t state_bytes, const mc_event* events, uint32_t count,
                                    uint32_t stride);

typedef struct mc_events_v1 {
    uint32_t size;
    uint32_t version;
    /* Replaces the subscription; kinds is a mask of MC_EVENT_MASK(kind). Every
       requested kind must be permitted. Zero kinds or a null handler unsubscribes. */
    mc_status (MC_CALL* subscribe)(void* binding, uint64_t generation, uint64_t kinds, mc_event_fn handler);
} mc_events_v1;

/* ---- mc.msg.render@1 ---------------------------------------------------- */
/*
 * Pull-style presentation decorators. The host calls the decorator once per
 * message revision (and again after invalidate) before laying the message
 * out, caches the merged result of every plugin, and never asks while
 * painting. A decorator returns a verdict, not pixels: plugins can't draw.
 * Across plugins: the strongest visibility and media verdict wins, prefixes
 * and suffixes join in plugin order, the first content replacement wins,
 * badges and accessories are appended up to the host limits.
 */
enum { MC_RENDER_NORMAL = 0, MC_RENDER_COLLAPSED = 1, MC_RENDER_HIDDEN = 2 };
enum { MC_MEDIA_SHOW = 0, MC_MEDIA_BLUR = 1, MC_MEDIA_HIDE = 2 };

typedef struct mc_render_message {
    uint32_t size;
    uint32_t flags; /* MC_MESSAGE_* */
    uint64_t channel_id;
    uint64_t message_id;
    uint64_t author_id;
    uint64_t guild_id;
    uint64_t timestamp_ms;
    uint64_t edited_ms;
    uint32_t revision;
    uint32_t relationship; /* MC_RELATIONSHIP_* between you and the author */
    uint32_t attachment_count;
    uint32_t embed_count;
    mc_str content;
    mc_str author_name;
} mc_render_message;

/* Host-zeroed output. Strings and node arrays are copied after the call. */
typedef struct mc_render_decoration {
    uint32_t size;
    uint32_t visibility;    /* MC_RENDER_*; collapsed rows show the label and a "Show" link */
    mc_str collapsed_label; /* <= 64 bytes; the host's default when empty */
    mc_str name_prefix;     /* <= 32 bytes, before the author name */
    mc_str name_suffix;     /* <= 32 bytes, after the author name */
    uint32_t name_tone;     /* MC_TONE_* (MC_TONE_ROLE: the author's role colour) */
    uint32_t media;         /* MC_MEDIA_* for attachments and embeds */
    uint32_t replace_content; /* non-zero: show content instead of the message text */
    uint32_t reserved;
    mc_str content;         /* display-only markdown source, <= 2000 bytes; never sent or copied as the message */
    const mc_slot_node* badges;      /* <= 2 badge/icon nodes after the author name */
    uint32_t badge_count;
    uint32_t badge_stride;
    const mc_slot_node* accessories; /* <= 4 label/button/link nodes in a row below the content */
    uint32_t accessory_count;
    uint32_t accessory_stride;
} mc_render_decoration;

typedef mc_status (MC_CALL* mc_render_fn)(void* state, uint32_t state_bytes, const mc_render_message* message,
                                          mc_render_decoration* decoration);

typedef struct mc_render_v1 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* set_decorator)(void* binding, uint64_t generation, mc_render_fn decorator);
    /* Asks again for one message (or every message when message_id is zero). */
    mc_status (MC_CALL* invalidate)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id);
    /* Accessory buttons report surface MC_SLOT_MESSAGE_ACCESSORY with the message ids. */
    mc_status (MC_CALL* set_action_handler)(void* binding, uint64_t generation, mc_slot_action_fn handler);
} mc_render_v1;

/* ---- mc.actions@1 ------------------------------------------------------- */
/*
 * Host-executed actions through MaiCord's normal paths; MC_OK means the
 * request was accepted and queued, and failures are shown by the regular UI
 * (a failed send stays in the chat with a retry). Rate limits per plugin:
 * messages (send/edit) 5 per 5 s, reactions 10 per 10 s, mark read 10 per
 * 10 s, presence (status/activity) 2 per 10 s, notices 3 per 10 s; one modal
 * at a time. Exhausted limits return MC_E_RATE_LIMITED.
 */
#define MC_SEND_SILENT 1u        /* @silent: no push notification */
#define MC_SEND_MENTION_REPLY 2u /* replies ping the replied-to author */
typedef struct mc_send_request {
    uint32_t size;
    uint32_t flags;      /* MC_SEND_* */
    uint64_t channel_id; /* must be sendable */
    uint64_t reply_to;   /* message id in channel_id, or zero */
    mc_str text;         /* 1..2000 bytes; not run through pre-send hooks or commands */
} mc_send_request;

/* A rich presence activity published with your status while set. Shown only
   when Settings > Activity sharing is on; it replaces the detected game. */
typedef struct mc_activity {
    uint32_t size;
    uint32_t type;           /* MC_ACTIVITY_PLAYING, _LISTENING, _WATCHING or _COMPETING */
    uint64_t application_id; /* optional */
    uint64_t start_ms;       /* optional */
    uint64_t end_ms;         /* optional */
    mc_str name;             /* 1..128 bytes */
    mc_str details;          /* <= 128 bytes */
    mc_str state;            /* <= 128 bytes */
} mc_activity;

/* A host dialog built from nodes: labels form the body, at most one INPUT
   (its text is the label, value the initial text), at most one BUTTON (its
   text is the accept label; MC_TONE_CRITICAL marks it dangerous). Links and
   separators are shown inline. Without an INPUT the dialog is a confirmation. */
typedef struct mc_modal {
    uint32_t size;
    uint32_t node_count; /* <= 8 */
    uint64_t modal_id;   /* echoed in the result */
    mc_str title;        /* <= 64 bytes */
    const mc_slot_node* nodes;
    uint32_t node_stride;
    uint32_t input_max_bytes; /* 1..2000; 0 means 256 */
} mc_modal;

typedef struct mc_modal_result {
    uint32_t size;
    uint32_t accepted; /* zero when cancelled or dismissed */
    uint64_t modal_id;
    mc_str text;       /* the input's text when accepted */
} mc_modal_result;

/* Runs inside a gesture. */
typedef mc_status (MC_CALL* mc_modal_fn)(void* state, uint32_t state_bytes, const mc_modal_result* result);

typedef struct mc_actions_v1 {
    uint32_t size;
    uint32_t version;
    /* [act_messages] */
    mc_status (MC_CALL* send_message)(void* binding, uint64_t generation, const mc_send_request* request);
    /* Your own messages only; the message must be in MaiCord's cache. */
    mc_status (MC_CALL* edit_message)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                                      mc_str text);
    /* emoji: a Unicode emoji, or "name:id" for a custom one. */
    mc_status (MC_CALL* react)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id,
                               mc_str emoji);
    mc_status (MC_CALL* mark_read)(void* binding, uint64_t generation, uint64_t channel_id);
    /* [act_presence] Empty text and emoji clear the custom status; expires_ms is a
       Unix time in milliseconds, zero for never. */
    mc_status (MC_CALL* set_custom_status)(void* binding, uint64_t generation, mc_str text, mc_str emoji,
                                           uint64_t expires_ms);
    /* Null clears this plugin's activity. */
    mc_status (MC_CALL* set_activity)(void* binding, uint64_t generation, const mc_activity* activity);
    /* [navigate] gesture only. message_id zero opens the channel. */
    mc_status (MC_CALL* open_channel)(void* binding, uint64_t generation, uint64_t channel_id, uint64_t message_id);
    mc_status (MC_CALL* open_profile)(void* binding, uint64_t generation, uint64_t user_id, uint64_t guild_id);
    /* Also needs [settings]: opens one of the plugin's own pages. */
    mc_status (MC_CALL* open_page)(void* binding, uint64_t generation, uint64_t page_id);
    /* [clipboard] gesture only; <= 16 KiB. */
    mc_status (MC_CALL* copy_text)(void* binding, uint64_t generation, mc_str text);
    /* [open_links] gesture only; https URLs <= 2048 bytes, always confirmed unless trusted. */
    mc_status (MC_CALL* open_url)(void* binding, uint64_t generation, mc_str url);
    /* [notify] A notice above the composer for 8 s; text <= 200 bytes. */
    mc_status (MC_CALL* notice)(void* binding, uint64_t generation, mc_str text, uint32_t tone);
    /* [notify] gesture only. The result arrives at the modal handler. */
    mc_status (MC_CALL* open_modal)(void* binding, uint64_t generation, const mc_modal* modal);
    mc_status (MC_CALL* set_modal_handler)(void* binding, uint64_t generation, mc_modal_fn handler);
} mc_actions_v1;

/* ---- mc.timer@1 --------------------------------------------------------- */

typedef void (MC_CALL* mc_timer_fn)(void* state, uint32_t state_bytes, uint64_t timer_id);

typedef struct mc_timer_v1 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* set_handler)(void* binding, uint64_t generation, mc_timer_fn handler);
    mc_status (MC_CALL* start)(void* binding, uint64_t generation, uint64_t timer_id, uint32_t interval_ms,
                               uint32_t repeat);
    mc_status (MC_CALL* cancel)(void* binding, uint64_t generation, uint64_t timer_id);
} mc_timer_v1;

/* ---- mc.fx@1 ------------------------------------------------------------ */
/*
 * Background effects drawn where MaiCord's Stars and Comets draw: over the
 * theme's background canvases, under every control. A registered effect
 * appears in Settings > Appearance > Background effect; the host draws only
 * the selected one, at most once per canvas per frame, and pauses time while
 * the window is hidden, minimized or covered, or with reduced motion.
 *
 * Bytecode: a DXBC container with a ps_5_0 pixel shader (fxc /T ps_5_0),
 * <= 64 KiB, validated and copied at registration. Its signature is
 *
 *   cbuffer McFx : register(b0) {
 *       float time, delta, intensity, dark;
 *       float2 resolution, origin;
 *       float4 palette[8];
 *       float4 parameters[2]; // parameters[i / 4][i % 4]; HLSL pads float arrays to rows
 *   };
 *   float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
 *
 * position is in window pixels, uv runs 0..1 across the canvas. Return a
 * premultiplied colour; it is composited over the canvas as
 * dst = src.rgb + dst * (1 - src.a). No textures, samplers or UAVs are bound.
 *
 * GPU time is measured per frame; an effect averaging more than 2 ms over a
 * second, or stalling a frame past 8 ms three times, is turned off with a
 * notice and the selection falls back to None.
 */
enum {
    MC_FX_BACKGROUND = 0, /* palette indices: theme roles in linear RGB, alpha 1 */
    MC_FX_SURFACE = 1,
    MC_FX_TEXT = 2,
    MC_FX_MUTED = 3,
    MC_FX_ACCENT = 4,
    MC_FX_POSITIVE = 5,
    MC_FX_WARNING = 6,
    MC_FX_CRITICAL = 7
};
#define MC_FX_PARAMETERS 8

/* Mirrors the HLSL cbuffer (16-byte rows); 192 bytes. */
typedef struct mc_fx_constants {
    float time;          /* seconds since the effect became active (paused while hidden) */
    float delta;         /* seconds since the previous frame */
    float intensity;     /* 0..1 strength; the host lowers it in light themes */
    float dark;          /* 1 in a dark theme, 0 in a light one */
    float resolution[2]; /* canvas size in pixels */
    float origin[2];     /* canvas top-left in window pixels */
    float palette[8][4]; /* MC_FX_* */
    float parameters[MC_FX_PARAMETERS]; /* set_parameters values */
} mc_fx_constants;

typedef struct mc_fx_effect {
    uint32_t size;
    uint32_t reserved;
    uint64_t effect_id; /* non-zero, unique per plugin */
    mc_str name;        /* 1..48 bytes, shown in the effect picker */
    const void* bytecode;
    uint32_t bytecode_bytes;
    uint32_t parameter_count; /* initial values follow in parameters */
    float parameters[MC_FX_PARAMETERS];
} mc_fx_effect;

typedef struct mc_fx_v1 {
    uint32_t size;
    uint32_t version;
    mc_status (MC_CALL* register_effect)(void* binding, uint64_t generation, const mc_fx_effect* effect);
    mc_status (MC_CALL* unregister_effect)(void* binding, uint64_t generation, uint64_t effect_id);
    /* count <= MC_FX_PARAMETERS finite values, written to parameters[0..count). */
    mc_status (MC_CALL* set_parameters)(void* binding, uint64_t generation, uint64_t effect_id, const float* values,
                                        uint32_t count);
} mc_fx_v1;

#ifdef __cplusplus
}
#endif

#endif /* MAICORD_PLUGIN_H */

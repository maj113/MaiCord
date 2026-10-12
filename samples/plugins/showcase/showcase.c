/*
 * MaiCord SDK sample "Showcase" (ABI 5). One small plugin that uses every
 * phase-1 capability, written the way a third-party plugin would be: plain
 * C11 against include/maicord/sdk/maicord_plugin.h, no CRT.
 *
 *   Message logger   mc.events keeps the last deleted and edited messages
 *                    (with the original text of edits) on its settings page
 *                    and in a composer popover.
 *   Name suffix      mc.msg.render shows "@username" after a display name that
 *                    differs from it, in the author's role tone; messages
 *                    from blocked users collapse; logged edits get an
 *                    accessory row with a "Show original" popover.
 *   Quick reply      a message menu item and a keybind open a host modal;
 *                    the text goes out as a reply through mc.actions.
 *   Menus            copy a user's ID or a link (mc.actions copy_text).
 *   Gradient         an mc.fx background effect with a speed parameter.
 *
 * Build: cl /c /Zl /GS- /W4 showcase.c; link /DLL /NOENTRY (or the entry
 * below) kernel32.lib; gradient.hlsl → fxc /T ps_5_0 /E main /Fh
 * showcase_gradient_ps.h /Vn g_showcase_gradient. The manifest lists the
 * permissions the user approves (showcase.maicord-plugin.json.in).
 */
#include "maicord/sdk/maicord_plugin.h"

typedef unsigned char BYTE;
#include "showcase_gradient_ps.h"

#define OWNER 0x4D43534844574341ull /* "MCSHOWCA" */
#define PAGE 1
#define S(text) {text, (uint32_t)(sizeof(text) - 1)}
#define LOG_ENTRIES 8
#define RECENT 32
#define EDITED 16

enum { CONTROL_SUFFIX = 2, CONTROL_BLOCKED = 3, CONTROL_KEYBIND = 5, CONTROL_SPEED = 6, CONTROL_CLEAR = 8, CONTROL_LOG = 100 };
enum { MENU_REPLY = 1, MENU_COPY_USER = 2, MENU_COPY_LINK = 3 };
enum { ACTION_LOG = 1, ACTION_OPEN_LOG = 2, ACTION_ORIGINAL = 3, ACTION_COPY_ORIGINAL = 4 };
enum { EFFECT_GRADIENT = 1 };

typedef struct log_entry {
    uint64_t channel_id, message_id;
    uint32_t edit; /* 0 deleted, 1 edited (text is the original) */
    uint32_t text_bytes, author_bytes, line_bytes;
    char text[120];
    char author[32];
    char line[176]; /* "Deleted · author: text" for the settings row */
} log_entry;

typedef struct recent_message {
    uint64_t message_id;
    uint32_t text_bytes;
    char text[120];
} recent_message;

typedef struct prefs {
    uint32_t suffix, collapse_blocked, speed_tenths, chord_bytes;
    char chord[64];
} prefs;

typedef struct showcase {
    void* binding;
    uint64_t generation;
    const mc_settings_v3* settings;
    const mc_read_v2* read;
    const mc_actions_v1* actions;
    const mc_render_v1* render;
    const mc_slots_v2* slots;
    const mc_fx_v1* fx;
    const mc_store_v1* store;
    prefs prefs;
    uint32_t log_count, log_next, recent_next, edited_next;
    log_entry log[LOG_ENTRIES];
    recent_message recent[RECENT];
    uint64_t edited[EDITED];
    uint64_t reply_channel, reply_message;
    uint64_t popover_message;
    char suffix[48];
    mc_slot_node nodes[8];   /* popover / accessory nodes live until the host copies them */
    mc_setting controls[8 + LOG_ENTRIES];
} showcase;

/* ---- Small freestanding helpers ------------------------------------------ */

static void zero(void* memory, uint32_t bytes) {
    uint32_t i;
    for (i = 0; i < bytes; ++i) ((char*)memory)[i] = 0;
}
static void copy(char* out, const char* in, uint32_t bytes) {
    uint32_t i;
    for (i = 0; i < bytes; ++i) out[i] = in[i];
}
/* Copies at most capacity bytes, cut on a UTF-8 boundary. */
static uint32_t put(char* out, uint32_t capacity, const char* in, uint32_t bytes) {
    if (bytes > capacity) {
        bytes = capacity;
        while (bytes && ((unsigned char)in[bytes] & 0xC0) == 0x80) --bytes;
    }
    copy(out, in, bytes);
    return bytes;
}
static uint32_t append(char* out, uint32_t at, uint32_t capacity, const char* in, uint32_t bytes) {
    return at + put(out + at, capacity - at, in, bytes);
}
static uint32_t decimal(char* out, uint64_t value) {
    char digits[24];
    uint32_t count = 0, at = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) out[at++] = digits[--count];
    return at;
}
static int same(mc_str a, const char* b, uint32_t bytes) {
    uint32_t i;
    if (a.bytes != bytes) return 0;
    for (i = 0; i < bytes; ++i)
        if (a.data[i] != b[i]) return 0;
    return 1;
}
static mc_slot_node node(uint32_t kind, mc_str text, uint64_t action) {
    mc_slot_node n;
    zero(&n, sizeof(n));
    n.size = sizeof(n);
    n.kind = kind;
    n.text = text;
    n.action_id = action;
    return n;
}

/* ---- Preferences and settings page --------------------------------------- */

static void save(showcase* s) {
    if (s->store) s->store->set(s->binding, s->generation, (mc_str)S("prefs"), &s->prefs, sizeof(s->prefs));
}
static mc_status publish(showcase* s) {
    mc_settings_page page;
    uint32_t count = 0, i;
    mc_setting* c = s->controls;
    zero(c, sizeof(s->controls));
    for (i = 0; i < sizeof(s->controls) / sizeof(s->controls[0]); ++i) { c[i].size = sizeof(mc_setting); c[i].enabled = 1; }
    c[count].kind = MC_SETTING_HEADER; c[count].id = 1; c[count++].label = (mc_str)S("Messages");
    c[count].kind = MC_SETTING_TOGGLE; c[count].id = CONTROL_SUFFIX; c[count].label = (mc_str)S("Show @username after display names");
    c[count++].checked = s->prefs.suffix;
    c[count].kind = MC_SETTING_TOGGLE; c[count].id = CONTROL_BLOCKED; c[count].label = (mc_str)S("Collapse messages from blocked users");
    c[count++].checked = s->prefs.collapse_blocked;
    c[count].kind = MC_SETTING_KEYBIND; c[count].id = CONTROL_KEYBIND; c[count].label = (mc_str)S("Quick reply shortcut");
    c[count].description = (mc_str)S("Replies to the newest message from someone else in the open channel.");
    c[count].text.data = s->prefs.chord; c[count++].text.bytes = s->prefs.chord_bytes;
    c[count].kind = MC_SETTING_SLIDER; c[count].id = CONTROL_SPEED; c[count].label = (mc_str)S("Gradient speed");
    c[count].minimum = 0; c[count].maximum = 20; c[count].step = 1; c[count++].value = (int32_t)s->prefs.speed_tenths;
    c[count].kind = MC_SETTING_HEADER; c[count].id = 7; c[count].label = (mc_str)S("Message log");
    c[count++].description = s->log_count ? (mc_str)S("Newest first. Copy puts the logged text on the clipboard.")
                                          : (mc_str)S("Deleted and edited messages appear here.");
    for (i = 0; i < s->log_count; ++i) {
        const log_entry* entry = &s->log[(s->log_next + LOG_ENTRIES - 1 - i) % LOG_ENTRIES];
        c[count].kind = MC_SETTING_NOTICE; c[count].id = CONTROL_LOG + i;
        c[count].label.data = entry->line; c[count].label.bytes = entry->line_bytes;
        c[count++].action_label = (mc_str)S("Copy");
    }
    c[count].kind = MC_SETTING_ACTION; c[count].id = CONTROL_CLEAR; c[count].label = (mc_str)S("Clear the log");
    c[count].enabled = s->log_count != 0; c[count++].action_label = (mc_str)S("Clear");
    zero(&page, sizeof(page));
    page.size = sizeof(page);
    page.control_count = count;
    page.title = (mc_str)S("Showcase");
    page.controls = c;
    page.control_stride = sizeof(mc_setting);
    return s->settings->set_page(s->binding, s->generation, PAGE, &page);
}
static void apply_speed(showcase* s) {
    float value = (float)s->prefs.speed_tenths / 10.0f;
    if (s->fx) s->fx->set_parameters(s->binding, s->generation, EFFECT_GRADIENT, &value, 1);
}

/* ---- Message logger (mc.events) ------------------------------------------- */

static recent_message* recent_find(showcase* s, uint64_t id) {
    uint32_t i;
    for (i = 0; i < RECENT; ++i)
        if (s->recent[i].message_id == id) return &s->recent[i];
    return 0;
}
static int edited(const showcase* s, uint64_t id) {
    uint32_t i;
    for (i = 0; i < EDITED; ++i)
        if (s->edited[i] == id) return 1;
    return 0;
}
static const log_entry* original_of(const showcase* s, uint64_t id) {
    uint32_t i;
    for (i = 0; i < s->log_count; ++i) {
        const log_entry* entry = &s->log[(s->log_next + LOG_ENTRIES - 1 - i) % LOG_ENTRIES];
        if (entry->edit && entry->message_id == id) return entry;
    }
    return 0;
}
static void log_add(showcase* s, const mc_event* event, uint32_t edit, mc_str text, mc_str author) {
    log_entry* entry = &s->log[s->log_next];
    uint32_t at = 0;
    zero(entry, sizeof(*entry));
    entry->channel_id = event->channel_id;
    entry->message_id = event->message_id;
    entry->edit = edit;
    entry->text_bytes = put(entry->text, sizeof(entry->text), text.data, text.bytes);
    entry->author_bytes = put(entry->author, sizeof(entry->author), author.data, author.bytes);
    at = edit ? append(entry->line, at, sizeof(entry->line), "Edited \xC2\xB7 ", 10)
              : append(entry->line, at, sizeof(entry->line), "Deleted \xC2\xB7 ", 11);
    at = append(entry->line, at, sizeof(entry->line), entry->author_bytes ? entry->author : "someone",
                entry->author_bytes ? entry->author_bytes : 7);
    at = append(entry->line, at, sizeof(entry->line), ": ", 2);
    at = append(entry->line, at, sizeof(entry->line), entry->text, entry->text_bytes);
    /* Settings labels are single line. */
    {
        uint32_t i;
        for (i = 0; i < at; ++i) if (entry->line[i] == '\n') entry->line[i] = ' ';
    }
    entry->line_bytes = at;
    s->log_next = (s->log_next + 1) % LOG_ENTRIES;
    if (s->log_count < LOG_ENTRIES) ++s->log_count;
}
static void MC_CALL on_events(void* state, uint32_t state_bytes, const mc_event* events, uint32_t count, uint32_t stride) {
    showcase* s = (showcase*)state;
    uint32_t i, changed = 0;
    (void)state_bytes;
    for (i = 0; i < count; ++i) {
        const mc_event* event = (const mc_event*)((const char*)events + (size_t)i * stride);
        if (event->kind == MC_EVENT_MESSAGE_CREATE && (event->flags & MC_EVENT_TEXT)) {
            recent_message* slot = &s->recent[s->recent_next];
            s->recent_next = (s->recent_next + 1) % RECENT;
            slot->message_id = event->message_id;
            slot->text_bytes = put(slot->text, sizeof(slot->text), event->text.data, event->text.bytes);
        } else if (event->kind == MC_EVENT_MESSAGE_DELETE && (event->flags & MC_EVENT_CACHED) && !(event->flags & MC_EVENT_OWN)) {
            log_add(s, event, 0, event->text, event->name);
            changed = 1;
        } else if (event->kind == MC_EVENT_MESSAGE_UPDATE && (event->flags & MC_EVENT_TEXT)) {
            recent_message* before = recent_find(s, event->message_id);
            if (!before || same(event->text, before->text, before->text_bytes)) continue; /* embeds only, or unknown */
            {
                mc_str original = {before->text, before->text_bytes};
                log_add(s, event, 1, original, event->name);
            }
            before->text_bytes = put(before->text, sizeof(before->text), event->text.data, event->text.bytes);
            if (!edited(s, event->message_id)) {
                s->edited[s->edited_next] = event->message_id;
                s->edited_next = (s->edited_next + 1) % EDITED;
            }
            if (s->render) s->render->invalidate(s->binding, s->generation, event->channel_id, event->message_id);
            changed = 1;
        }
    }
    if (changed) publish(s);
}

/* ---- Name suffix decorator (mc.msg.render) -------------------------------- */

static mc_status MC_CALL on_decorate(void* state, uint32_t state_bytes, const mc_render_message* message,
                                     mc_render_decoration* out) {
    showcase* s = (showcase*)state;
    (void)state_bytes;
    if (s->prefs.collapse_blocked && message->relationship == MC_RELATIONSHIP_BLOCKED) {
        out->visibility = MC_RENDER_COLLAPSED;
        out->collapsed_label = (mc_str)S("Message from a blocked user");
        return MC_OK;
    }
    if (s->prefs.suffix && s->read) {
        mc_user_info user;
        zero(&user, sizeof(user));
        user.size = sizeof(user);
        if (s->read->user(s->binding, s->generation, message->author_id, &user) == MC_OK && user.username_bytes &&
            !same(message->author_name, user.username, user.username_bytes)) {
            uint32_t at = append(s->suffix, 0, sizeof(s->suffix), "@", 1);
            at = append(s->suffix, at, 32, user.username, user.username_bytes);
            out->name_suffix.data = s->suffix;
            out->name_suffix.bytes = at;
            out->name_tone = MC_TONE_ROLE;
        }
    }
    if (edited(s, message->message_id) && original_of(s, message->message_id)) {
        s->nodes[0] = node(MC_NODE_LABEL, (mc_str)S("Edited \xC2\xB7 original logged"), 0);
        s->nodes[1] = node(MC_NODE_BUTTON, (mc_str)S("Show original"), ACTION_ORIGINAL);
        out->accessories = s->nodes;
        out->accessory_count = 2;
        out->accessory_stride = sizeof(mc_slot_node);
    }
    return MC_OK;
}

/* Accessory and slot buttons share one handler shape; popover nodes stay in state. */
static mc_status MC_CALL on_accessory(void* state, uint32_t state_bytes, const mc_slot_event* event, mc_popover* popover) {
    showcase* s = (showcase*)state;
    const log_entry* entry = original_of(s, event->message_id);
    (void)state_bytes;
    if (!entry) return MC_E_NOT_FOUND;
    if (event->action_id == ACTION_COPY_ORIGINAL)
        return s->actions->copy_text(s->binding, s->generation, (mc_str){entry->text, entry->text_bytes});
    if (event->action_id != ACTION_ORIGINAL) return MC_E_INVALID;
    s->nodes[0] = node(MC_NODE_LABEL, (mc_str){entry->text, entry->text_bytes}, 0);
    s->nodes[1] = node(MC_NODE_SEPARATOR, (mc_str){0, 0}, 0);
    s->nodes[2] = node(MC_NODE_BUTTON, (mc_str)S("Copy original"), ACTION_COPY_ORIGINAL);
    popover->nodes = s->nodes;
    popover->node_count = 3;
    popover->node_stride = sizeof(mc_slot_node);
    return MC_OK;
}

static mc_status MC_CALL on_slot(void* state, uint32_t state_bytes, const mc_slot_event* event, mc_popover* popover) {
    showcase* s = (showcase*)state;
    uint32_t i, count = 0;
    (void)state_bytes;
    if (event->action_id == ACTION_OPEN_LOG) return s->actions->open_page(s->binding, s->generation, PAGE);
    if (event->surface != MC_SLOT_COMPOSER_TOOLBAR || event->action_id != ACTION_LOG) return MC_E_INVALID;
    if (!s->log_count) s->nodes[count++] = node(MC_NODE_LABEL, (mc_str)S("Nothing logged yet."), 0);
    for (i = 0; i < s->log_count && i < 4; ++i) {
        const log_entry* entry = &s->log[(s->log_next + LOG_ENTRIES - 1 - i) % LOG_ENTRIES];
        s->nodes[count++] = node(MC_NODE_LABEL, (mc_str){entry->line, entry->line_bytes}, 0);
    }
    s->nodes[count++] = node(MC_NODE_SEPARATOR, (mc_str){0, 0}, 0);
    s->nodes[count++] = node(MC_NODE_BUTTON, (mc_str)S("Open the log"), ACTION_OPEN_LOG);
    popover->nodes = s->nodes;
    popover->node_count = count;
    popover->node_stride = sizeof(mc_slot_node);
    return MC_OK;
}

/* ---- Quick reply (menu + keybind → modal → send) --------------------------- */

static mc_status open_quick_reply(showcase* s, uint64_t channel, uint64_t message) {
    mc_message_info info;
    mc_modal modal;
    char quote[96];
    char title[64];
    uint32_t at = 0, title_bytes;
    mc_status read;
    zero(&info, sizeof(info));
    info.size = sizeof(info);
    info.content = quote;
    info.content_capacity = sizeof(quote);
    read = s->read->message(s->binding, s->generation, channel, message, &info);
    if ((read != MC_OK && read != MC_E_TRUNCATED) || info.message_id != message) return MC_E_NOT_FOUND;
    at = info.content_bytes < sizeof(quote) ? info.content_bytes : sizeof(quote);
    title_bytes = append(title, 0, sizeof(title), "Reply to ", 9);
    title_bytes = append(title, title_bytes, sizeof(title), info.author_name_bytes ? info.author_name : "message",
                         info.author_name_bytes ? info.author_name_bytes : 7);
    s->reply_channel = channel;
    s->reply_message = message;
    s->nodes[0] = node(MC_NODE_LABEL, (mc_str){quote, at}, 0);
    s->nodes[1] = node(MC_NODE_INPUT, (mc_str)S("Reply"), 0);
    s->nodes[2] = node(MC_NODE_BUTTON, (mc_str)S("Send"), 1);
    zero(&modal, sizeof(modal));
    modal.size = sizeof(modal);
    modal.node_count = at ? 3 : 2;
    modal.modal_id = message;
    modal.title.data = title;
    modal.title.bytes = title_bytes;
    modal.nodes = at ? s->nodes : s->nodes + 1;
    modal.node_stride = sizeof(mc_slot_node);
    modal.input_max_bytes = 2000;
    return s->actions->open_modal(s->binding, s->generation, &modal);
}
static mc_status MC_CALL on_modal(void* state, uint32_t state_bytes, const mc_modal_result* result) {
    showcase* s = (showcase*)state;
    mc_send_request request;
    mc_status sent;
    (void)state_bytes;
    if (!result->accepted || !result->text.bytes || result->modal_id != s->reply_message) return MC_OK;
    zero(&request, sizeof(request));
    request.size = sizeof(request);
    request.channel_id = s->reply_channel;
    request.reply_to = s->reply_message;
    request.flags = MC_SEND_MENTION_REPLY;
    request.text = result->text;
    sent = s->actions->send_message(s->binding, s->generation, &request);
    if (sent == MC_E_RATE_LIMITED)
        s->actions->notice(s->binding, s->generation, (mc_str)S("Quick reply is sending too fast; try again in a moment."),
                           MC_TONE_WARNING);
    return sent;
}
static void MC_CALL on_keybind(void* state, uint32_t state_bytes, uint64_t page_id, uint64_t control_id) {
    showcase* s = (showcase*)state;
    mc_context_info context;
    uint64_t ids[16];
    uint32_t count = 0, next = 0, i;
    (void)state_bytes;
    if (page_id != PAGE || control_id != CONTROL_KEYBIND) return;
    zero(&context, sizeof(context));
    context.size = sizeof(context);
    if (s->read->context(s->binding, s->generation, &context) != MC_OK || !context.channel_id) return;
    if (s->read->list(s->binding, s->generation, MC_LIST_MESSAGES, context.channel_id, 0, ids, 16, &count, &next) != MC_OK)
        return;
    for (i = 0; i < count; ++i) {
        mc_message_info info;
        zero(&info, sizeof(info));
        info.size = sizeof(info);
        if (s->read->message(s->binding, s->generation, context.channel_id, ids[i], &info) != MC_OK) continue;
        if (info.flags & MC_MESSAGE_OWN) continue;
        open_quick_reply(s, context.channel_id, ids[i]);
        return;
    }
    s->actions->notice(s->binding, s->generation, (mc_str)S("No message to reply to here."), MC_TONE_NEUTRAL);
}

static mc_status MC_CALL on_menu(void* state, uint32_t state_bytes, const mc_menu_context* context) {
    showcase* s = (showcase*)state;
    char digits[24];
    (void)state_bytes;
    switch (context->item_id) {
    case MENU_REPLY: return open_quick_reply(s, context->channel_id, context->message_id);
    case MENU_COPY_USER: return s->actions->copy_text(s->binding, s->generation, (mc_str){digits, decimal(digits, context->user_id)});
    case MENU_COPY_LINK: return s->actions->copy_text(s->binding, s->generation, context->url);
    default: return MC_E_NOT_FOUND;
    }
}

/* ---- Settings handler ----------------------------------------------------- */

static mc_status MC_CALL on_setting(void* state, uint32_t state_bytes, const mc_setting_value* value) {
    showcase* s = (showcase*)state;
    (void)state_bytes;
    switch (value->control_id) {
    case CONTROL_SUFFIX: s->prefs.suffix = value->checked; break;
    case CONTROL_BLOCKED: s->prefs.collapse_blocked = value->checked; break;
    case CONTROL_KEYBIND: s->prefs.chord_bytes = put(s->prefs.chord, sizeof(s->prefs.chord), value->text.data, value->text.bytes); break;
    case CONTROL_SPEED: s->prefs.speed_tenths = (uint32_t)value->value; apply_speed(s); break;
    case CONTROL_CLEAR: s->log_count = s->log_next = 0; zero(s->edited, sizeof(s->edited)); break;
    default:
        if (value->control_id >= CONTROL_LOG && value->control_id < CONTROL_LOG + s->log_count) {
            const log_entry* entry = &s->log[(s->log_next + LOG_ENTRIES - 1 - (uint32_t)(value->control_id - CONTROL_LOG)) % LOG_ENTRIES];
            return s->actions->copy_text(s->binding, s->generation, (mc_str){entry->text, entry->text_bytes});
        }
        return MC_E_NOT_FOUND;
    }
    save(s);
    if (s->render) s->render->invalidate(s->binding, s->generation, 0, 0);
    return publish(s);
}

/* ---- Lifecycle -------------------------------------------------------------- */

static mc_status MC_CALL start(const mc_host* host, void* state, uint32_t state_bytes) {
    showcase* s = (showcase*)state;
    const mc_pages_v1* pages = 0;
    const mc_menus_v2* menus = 0;
    const mc_events_v1* events = 0;
    uint32_t got = 0;
    mc_slot_target toolbar;
    mc_slot_content content;
    mc_slot_node button;
    mc_fx_effect effect;
    if (state_bytes < sizeof(showcase) || !MC_HAS_FIELD(host, mc_host, get_interface) || host->abi_version < 5)
        return MC_E_UNSUPPORTED;
    s->binding = host->binding;
    s->generation = host->generation;
#define FETCH(name, version, out) \
    if (host->get_interface(host->binding, host->generation, name, version, (const void**)&(out)) != MC_OK) return MC_E_UNSUPPORTED
    FETCH("mc.ui.pages", 1, pages);
    FETCH("mc.settings", 3, s->settings);
    FETCH("mc.ui.menus", 2, menus);
    FETCH("mc.read", 2, s->read);
    FETCH("mc.events", 1, events);
    FETCH("mc.msg.render", 1, s->render);
    FETCH("mc.actions", 1, s->actions);
    FETCH("mc.ui.slots", 2, s->slots);
    FETCH("mc.store", 1, s->store);
    FETCH("mc.fx", 1, s->fx);
#undef FETCH
    /* Defaults, then whatever the user chose last time. */
    s->prefs.suffix = 1;
    s->prefs.collapse_blocked = 1;
    s->prefs.speed_tenths = 5;
    s->prefs.chord_bytes = put(s->prefs.chord, sizeof(s->prefs.chord), "Ctrl+Shift+R", 12);
    {
        prefs stored;
        if (s->store->get(s->binding, s->generation, (mc_str)S("prefs"), &stored, sizeof(stored), &got) == MC_OK &&
            got == sizeof(stored) && stored.chord_bytes <= sizeof(stored.chord) && stored.speed_tenths <= 20)
            s->prefs = stored;
    }
    if (pages->register_page(s->binding, s->generation, PAGE, (mc_str)S("Showcase")) != MC_OK) return MC_E_INVALID;
    s->settings->set_handler(s->binding, s->generation, on_setting);
    s->settings->set_keybind_handler(s->binding, s->generation, on_keybind);
    if (publish(s) != MC_OK) return MC_E_INVALID;
    menus->register_item(s->binding, s->generation, MENU_REPLY, MC_MENU_MESSAGE, (mc_str)S("Quick reply"), 0);
    menus->register_item(s->binding, s->generation, MENU_COPY_USER, MC_MENU_USER, (mc_str)S("Copy user ID"), 0);
    menus->register_item(s->binding, s->generation, MENU_COPY_LINK, MC_MENU_LINK, (mc_str)S("Copy link"), 0);
    menus->set_handler(s->binding, s->generation, on_menu);
    if (events->subscribe(s->binding, s->generation,
                          MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE) | MC_EVENT_MASK(MC_EVENT_MESSAGE_UPDATE) |
                              MC_EVENT_MASK(MC_EVENT_MESSAGE_DELETE), on_events) != MC_OK) return MC_E_INVALID;
    s->render->set_decorator(s->binding, s->generation, on_decorate);
    s->render->set_action_handler(s->binding, s->generation, on_accessory);
    s->actions->set_modal_handler(s->binding, s->generation, on_modal);
    zero(&toolbar, sizeof(toolbar));
    toolbar.size = sizeof(toolbar);
    toolbar.surface = MC_SLOT_COMPOSER_TOOLBAR;
    button = node(MC_NODE_BUTTON, (mc_str){0, 0}, ACTION_LOG);
    button.icon = (mc_str)S("lucide/history");
    button.tooltip = (mc_str)S("Message log");
    zero(&content, sizeof(content));
    content.size = sizeof(content);
    content.node_count = 1;
    content.nodes = &button;
    content.node_stride = sizeof(button);
    s->slots->set(s->binding, s->generation, &toolbar, &content);
    s->slots->set_handler(s->binding, s->generation, on_slot);
    zero(&effect, sizeof(effect));
    effect.size = sizeof(effect);
    effect.effect_id = EFFECT_GRADIENT;
    effect.name = (mc_str)S("Showcase gradient");
    effect.bytecode = g_showcase_gradient;
    effect.bytecode_bytes = sizeof(g_showcase_gradient);
    effect.parameter_count = 1;
    effect.parameters[0] = (float)s->prefs.speed_tenths / 10.0f;
    if (s->fx->register_effect(s->binding, s->generation, &effect) != MC_OK) return MC_E_INVALID;
    return MC_OK;
}

static void MC_CALL stop(void* state, uint32_t state_bytes) {
    (void)state; (void)state_bytes; /* the host drops every registration */
}

MC_EXPORT mc_status MC_CALL maicord_plugin_query_v5(const mc_host_info* host, mc_plugin_definition* definition) {
    mc_plugin_definition local;
    if (!MC_HAS_FIELD(host, mc_host_info, abi_version) || host->abi_version < 5) return MC_E_UNSUPPORTED;
    zero(&local, sizeof(local));
    local.size = sizeof(local);
    local.abi_version = MC_ABI_VERSION;
    local.owner_id = OWNER;
    local.permissions = MC_PERM_SETTINGS | MC_PERM_MENUS | MC_PERM_UI_SLOTS | MC_PERM_STORE | MC_PERM_READ_CONTEXT |
                        MC_PERM_READ_MEMBERS | MC_PERM_READ_MESSAGES | MC_PERM_MESSAGE_RENDER | MC_PERM_ACT_MESSAGES |
                        MC_PERM_NAVIGATE | MC_PERM_CLIPBOARD | MC_PERM_NOTIFY | MC_PERM_KEYBINDS | MC_PERM_EFFECTS;
    local.state_bytes = sizeof(showcase);
    local.state_alignment = 8;
    local.min_host_abi = 5;
    local.start = start;
    local.stop = stop;
    return mc_definition_publish(definition, &local);
}

int MC_CALL maicord_showcase_entry(void* instance, unsigned long reason, void* reserved) {
    (void)instance; (void)reason; (void)reserved;
    return 1;
}

/* ABI-5 test plugin exercising the core interfaces. Built only for tests. */
#include "maicord/sdk/maicord_plugin.h"

#define OWNER 0x4D43344655554C4Cull
#define S(text) {text, (uint32_t)(sizeof(text) - 1)}

typedef struct fixture_state {
    void* binding;
    uint64_t generation;
    const mc_settings_v3* settings;
    const mc_slots_v2* slots;
    const mc_store_v1* store;
    const mc_read_v2* read;
    uint32_t toggle, choice, colour, notice_presses, actions, menu_presses;
    int32_t slider;
    char text[64];
    uint32_t text_bytes;
    char keybind[64];
    uint32_t keybind_bytes;
    uint32_t observed, timer_fires, slot_actions, launches, popover_presses;
    uint64_t seed;
} fixture_state;

static void copy(char* out, const char* in, uint32_t bytes) {
    uint32_t i;
    for (i = 0; i < bytes; ++i) out[i] = in[i];
}
static uint32_t append(char* out, uint32_t at, const char* text, uint32_t bytes) {
    copy(out + at, text, bytes);
    return at + bytes;
}
static uint32_t append_number(char* out, uint32_t at, uint64_t value) {
    char digits[24];
    uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) out[at++] = digits[--count];
    return at;
}

static mc_status publish(fixture_state* s) {
    static const mc_str choices[3] = {S("Red"), S("Green"), S("Blue")};
    mc_setting controls[9];
    mc_settings_page page;
    uint32_t i;
    char* bytes = (char*)controls;
    for (i = 0; i < sizeof(controls); ++i) bytes[i] = 0;
    for (i = 0; i < 9; ++i) { controls[i].size = sizeof(mc_setting); controls[i].enabled = 1; controls[i].id = i + 1; }
    controls[0].kind = MC_SETTING_HEADER; controls[0].label = (mc_str)S("Dice");
    controls[1].kind = MC_SETTING_TOGGLE; controls[1].label = (mc_str)S("Exploding dice"); controls[1].checked = s->toggle;
    controls[2].kind = MC_SETTING_CHOICE; controls[2].label = (mc_str)S("Colour");
    controls[2].choices = choices; controls[2].choice_count = 3; controls[2].selected = s->choice;
    controls[3].kind = MC_SETTING_ACTION; controls[3].label = (mc_str)S("Reset"); controls[3].action_label = (mc_str)S("Reset");
    controls[4].kind = MC_SETTING_TEXT; controls[4].label = (mc_str)S("Prefix");
    controls[4].text.data = s->text; controls[4].text.bytes = s->text_bytes; controls[4].max_bytes = 32;
    controls[5].kind = MC_SETTING_SLIDER; controls[5].label = (mc_str)S("Sides");
    controls[5].minimum = 0; controls[5].maximum = 100; controls[5].step = 5; controls[5].value = s->slider;
    controls[6].kind = MC_SETTING_COLOUR; controls[6].label = (mc_str)S("Badge colour"); controls[6].colour = s->colour;
    controls[7].kind = MC_SETTING_KEYBIND; controls[7].label = (mc_str)S("Roll shortcut");
    controls[7].text.data = s->keybind; controls[7].text.bytes = s->keybind_bytes;
    controls[8].kind = MC_SETTING_NOTICE; controls[8].label = (mc_str)S("Ready to roll");
    controls[8].action_label = (mc_str)S("Test");
    bytes = (char*)&page;
    for (i = 0; i < sizeof(page); ++i) bytes[i] = 0;
    page.size = sizeof(page);
    page.control_count = 9;
    page.title = (mc_str)S("Dice settings");
    page.controls = controls;
    page.control_stride = sizeof(mc_setting);
    return s->settings->set_page(s->binding, s->generation, 1, &page);
}

static mc_status MC_CALL on_setting(void* state, uint32_t bytes, const mc_setting_value* value) {
    fixture_state* s = (fixture_state*)state;
    (void)bytes;
    switch (value->kind) {
    case MC_SETTING_TOGGLE: s->toggle = value->checked; break;
    case MC_SETTING_CHOICE: s->choice = value->selected; break;
    case MC_SETTING_ACTION: ++s->actions; break;
    case MC_SETTING_SLIDER: s->slider = value->value; break;
    case MC_SETTING_COLOUR: s->colour = value->colour; break;
    case MC_SETTING_NOTICE: ++s->notice_presses; break;
    case MC_SETTING_TEXT:
        if (value->text.bytes > sizeof(s->text)) return MC_E_INVALID;
        copy(s->text, value->text.data, value->text.bytes); s->text_bytes = value->text.bytes; break;
    case MC_SETTING_KEYBIND:
        if (value->text.bytes > sizeof(s->keybind)) return MC_E_INVALID;
        copy(s->keybind, value->text.data, value->text.bytes); s->keybind_bytes = value->text.bytes; break;
    default: return MC_E_UNSUPPORTED;
    }
    return publish(s);
}

static mc_status MC_CALL on_menu(void* state, uint32_t bytes, const mc_menu_context* target) {
    fixture_state* s = (fixture_state*)state;
    (void)bytes;
    if (target->target != MC_MENU_MESSAGE || target->item_id != 1 || !target->message_id) return MC_E_INVALID;
    ++s->menu_presses;
    return MC_OK;
}

/* "!shout x" uppercases x, "!cancel" cancels, "!overflow" claims too many bytes. */
static mc_status MC_CALL on_pre_send(void* state, uint32_t bytes, const mc_message_view* draft, mc_text_output* out) {
    uint32_t i;
    (void)state; (void)bytes;
    if (draft->content.bytes == 7 && draft->content.data[0] == '!' && draft->content.data[1] == 'c') {
        out->action = MC_SEND_CANCEL;
        return MC_OK;
    }
    if (draft->content.bytes == 9 && draft->content.data[0] == '!' && draft->content.data[1] == 'o') {
        out->action = MC_SEND_REPLACE;
        out->bytes = out->capacity + 1;
        return MC_OK;
    }
    if (draft->content.bytes > 7 && draft->content.data[0] == '!' && draft->content.data[1] == 's') {
        if (draft->content.bytes - 7 > out->capacity) return MC_OK;
        for (i = 7; i < draft->content.bytes; ++i) {
            char c = draft->content.data[i];
            out->data[i - 7] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
        out->bytes = draft->content.bytes - 7;
        out->action = MC_SEND_REPLACE;
    }
    return MC_OK;
}

static void tag_roll(fixture_state* s, const mc_event* event) {
    uint32_t i;
    ++s->observed;
    for (i = 0; i + 4 <= event->text.bytes; ++i)
        if (event->text.data[i] == 'r' && event->text.data[i + 1] == 'o' &&
            event->text.data[i + 2] == 'l' && event->text.data[i + 3] == 'l') {
            mc_slot_target target = {sizeof(mc_slot_target), MC_SLOT_MESSAGE_ACCESSORY, 0, 0, 0, 0};
            mc_slot_node nodes[2];
            mc_slot_content content;
            char* raw = (char*)nodes;
            uint32_t j;
            for (j = 0; j < sizeof(nodes); ++j) raw[j] = 0;
            target.channel_id = event->channel_id;
            target.message_id = event->message_id;
            nodes[0].size = nodes[1].size = sizeof(mc_slot_node);
            nodes[0].kind = MC_NODE_BADGE; nodes[0].text = (mc_str)S("rolled"); nodes[0].tone = MC_TONE_ACCENT;
            nodes[0].icon = (mc_str)S("lucide/star"); nodes[0].tooltip = (mc_str)S("A dice roll");
            nodes[1].kind = MC_NODE_BUTTON; nodes[1].icon = (mc_str)S("lucide/does-not-exist");
            nodes[1].text = (mc_str)S("Reroll"); nodes[1].action_id = 7; nodes[1].tone = 99;
            content.size = sizeof(content); content.node_count = 2; content.nodes = nodes;
            content.node_stride = sizeof(mc_slot_node); content.reserved = 0;
            s->slots->set(s->binding, s->generation, &target, &content);
            return;
        }
}

static void MC_CALL on_events(void* state, uint32_t bytes, const mc_event* events, uint32_t count, uint32_t stride) {
    fixture_state* s = (fixture_state*)state;
    uint32_t i;
    (void)bytes;
    for (i = 0; i < count; ++i) {
        const mc_event* event = (const mc_event*)((const char*)events + (size_t)i * stride);
        if (event->kind == MC_EVENT_MESSAGE_CREATE) tag_roll(s, event);
    }
}

/* Reroll (7) answers with a two-row popover; its "Again" row (8) counts popover presses. */
static mc_status MC_CALL on_slot(void* state, uint32_t bytes, const mc_slot_event* event, mc_popover* popover) {
    static mc_slot_node rows[2];
    fixture_state* s = (fixture_state*)state;
    char* raw = (char*)rows;
    uint32_t j;
    (void)bytes;
    if (event->surface != MC_SLOT_MESSAGE_ACCESSORY) return MC_E_INVALID;
    if (event->action_id == 8) { ++s->popover_presses; return MC_OK; }
    if (event->action_id != 7) return MC_E_INVALID;
    ++s->slot_actions;
    for (j = 0; j < sizeof(rows); ++j) raw[j] = 0;
    rows[0].size = rows[1].size = sizeof(mc_slot_node);
    rows[0].kind = MC_NODE_LABEL; rows[0].text = (mc_str)S("Roll again?");
    rows[1].kind = MC_NODE_BUTTON; rows[1].text = (mc_str)S("Again"); rows[1].action_id = 8;
    popover->node_count = 2; popover->nodes = rows; popover->node_stride = sizeof(mc_slot_node);
    return MC_OK;
}

static uint32_t next_random(fixture_state* s, uint32_t sides) {
    s->seed = s->seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)((s->seed >> 33) % sides) + 1;
}

/* /roll [NdM] sends "rolled NdM: a + b = total"; /repeat times loud text. */
static mc_status MC_CALL on_command(void* state, uint32_t bytes, const mc_command_invocation* call, mc_text_output* out) {
    fixture_state* s = (fixture_state*)state;
    const mc_command_argument* arguments = call->arguments;
    uint32_t at = 0, i;
    (void)bytes;
    if (call->command_id == 1) {
        uint32_t count = 1, sides = 6, total = 0;
        uint32_t stored = 0, got = 0;
        mc_str key = S("rolls");
        if (call->argument_count && arguments[0].present) {
            const char* text = arguments[0].text.data;
            uint32_t n = arguments[0].text.bytes, p = 0;
            count = 0; sides = 0;
            while (p < n && text[p] >= '0' && text[p] <= '9') count = count * 10 + (uint32_t)(text[p++] - '0');
            if (p >= n || text[p] != 'd') { out->action = MC_CMD_NOTICE; at = append(out->data, 0, "Use NdM, like 2d6.", 18); out->bytes = at; return MC_OK; }
            ++p;
            while (p < n && text[p] >= '0' && text[p] <= '9') sides = sides * 10 + (uint32_t)(text[p++] - '0');
            if (!count || count > 20 || sides < 2 || sides > 1000) return MC_E_INVALID;
        }
        at = append(out->data, at, "rolled ", 7);
        at = append_number(out->data, at, count);
        out->data[at++] = 'd';
        at = append_number(out->data, at, sides);
        at = append(out->data, at, ": ", 2);
        for (i = 0; i < count; ++i) {
            uint32_t value = next_random(s, sides);
            total += value;
            if (i) at = append(out->data, at, " + ", 3);
            at = append_number(out->data, at, value);
        }
        at = append(out->data, at, " = ", 3);
        at = append_number(out->data, at, total);
        out->bytes = at;
        out->action = MC_CMD_SEND;
        if (s->store->get(s->binding, s->generation, key, &stored, sizeof(stored), &got) != MC_OK) stored = 0;
        ++stored;
        s->store->set(s->binding, s->generation, key, &stored, sizeof(stored));
        return MC_OK;
    }
    if (call->command_id == 2) {
        int64_t times = arguments[0].integer;
        uint32_t loud = arguments[1].present && arguments[1].boolean;
        mc_str text = arguments[2].text;
        int64_t k;
        if (times < 1 || times > 5) { out->action = MC_CMD_NOTICE; out->bytes = append(out->data, 0, "1 to 5 times.", 13); return MC_OK; }
        for (k = 0; k < times; ++k) {
            if (k) out->data[at++] = ' ';
            for (i = 0; i < text.bytes; ++i) {
                char c = text.data[i];
                out->data[at++] = loud && c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
            }
        }
        out->bytes = at;
        out->action = MC_CMD_REPLACE;
        return MC_OK;
    }
    return MC_E_NOT_FOUND;
}

static void MC_CALL on_timer(void* state, uint32_t bytes, uint64_t id) {
    fixture_state* s = (fixture_state*)state;
    (void)bytes;
    if (id == 1) ++s->timer_fires;
}

static mc_status MC_CALL start(const mc_host* host, void* state, uint32_t state_bytes) {
    fixture_state* s = (fixture_state*)state;
    const mc_pages_v1* pages = 0;
    const mc_menus_v2* menus = 0;
    const mc_msg_hooks_v2* hooks = 0;
    const mc_events_v1* events = 0;
    const mc_cmd_v1* commands = 0;
    const mc_timer_v1* timer = 0;
    mc_command_option repeat_options[3];
    mc_command_option roll_option;
    mc_command roll, repeat;
    mc_slot_target toolbar = {sizeof(mc_slot_target), MC_SLOT_COMPOSER_TOOLBAR, 0, 0, 0, 0};
    mc_slot_node node;
    mc_slot_content content;
    mc_str launches_key = S("launches");
    uint32_t got = 0, i;
    char* raw;
    if (state_bytes < sizeof(fixture_state) || !MC_HAS_FIELD(host, mc_host, get_interface)) return MC_E_INVALID;
    s->binding = host->binding;
    s->generation = host->generation;
    s->seed = 12345;
    s->slider = 50;
    s->colour = 0x5865F2;
#define FETCH(name, version, out) \
    if (host->get_interface(host->binding, host->generation, name, version, (const void**)&(out)) != MC_OK) return MC_E_UNSUPPORTED
    FETCH("mc.ui.pages", 1, pages);
    FETCH("mc.settings", 3, s->settings);
    FETCH("mc.ui.menus", 2, menus);
    FETCH("mc.msg.hooks", 2, hooks);
    FETCH("mc.events", 1, events);
    FETCH("mc.cmd", 1, commands);
    FETCH("mc.ui.slots", 2, s->slots);
    FETCH("mc.store", 1, s->store);
    FETCH("mc.read", 2, s->read);
    FETCH("mc.timer", 1, timer);
#undef FETCH
    if (!MC_HAS_FIELD(s->store, mc_store_v1, usage)) return MC_E_UNSUPPORTED;
    if (pages->register_page(s->binding, s->generation, 1, (mc_str)S("Dice")) != MC_OK) return MC_E_INVALID;
    if (publish(s) != MC_OK) return MC_E_INVALID;
    s->settings->set_handler(s->binding, s->generation, on_setting);
    if (menus->register_item(s->binding, s->generation, 1, MC_MENU_MESSAGE, (mc_str)S("Roll for this message"), 0) != MC_OK)
        return MC_E_INVALID;
    menus->set_handler(s->binding, s->generation, on_menu);
    hooks->set_pre_send(s->binding, s->generation, on_pre_send);
    if (events->subscribe(s->binding, s->generation, MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE), on_events) != MC_OK)
        return MC_E_INVALID;
    raw = (char*)&roll_option;
    for (i = 0; i < sizeof(roll_option); ++i) raw[i] = 0;
    roll_option.size = sizeof(roll_option); roll_option.type = MC_OPTION_STRING;
    roll_option.name = (mc_str)S("dice"); roll_option.description = (mc_str)S("Dice like 2d6");
    raw = (char*)&roll;
    for (i = 0; i < sizeof(roll); ++i) raw[i] = 0;
    roll.size = sizeof(roll); roll.command_id = 1; roll.name = (mc_str)S("roll");
    roll.description = (mc_str)S("Roll some dice"); roll.option_count = 1; roll.options = &roll_option;
    roll.option_stride = sizeof(mc_command_option);
    if (commands->register_command(s->binding, s->generation, &roll) != MC_OK) return MC_E_INVALID;
    raw = (char*)repeat_options;
    for (i = 0; i < sizeof(repeat_options); ++i) raw[i] = 0;
    for (i = 0; i < 3; ++i) repeat_options[i].size = sizeof(mc_command_option);
    repeat_options[0].type = MC_OPTION_INTEGER; repeat_options[0].name = (mc_str)S("times"); repeat_options[0].required = 1;
    repeat_options[1].type = MC_OPTION_BOOLEAN; repeat_options[1].name = (mc_str)S("loud");
    repeat_options[2].type = MC_OPTION_STRING; repeat_options[2].name = (mc_str)S("text"); repeat_options[2].required = 1;
    repeat = roll;
    repeat.command_id = 2; repeat.name = (mc_str)S("repeat"); repeat.description = (mc_str)S("Repeat text");
    repeat.option_count = 3; repeat.options = repeat_options;
    if (commands->register_command(s->binding, s->generation, &repeat) != MC_OK) return MC_E_INVALID;
    commands->set_handler(s->binding, s->generation, on_command);
    raw = (char*)&node;
    for (i = 0; i < sizeof(node); ++i) raw[i] = 0;
    node.size = sizeof(node); node.kind = MC_NODE_BUTTON; node.icon = (mc_str)S("lucide/star"); node.action_id = 9;
    node.tooltip = (mc_str)S("Roll");
    content.size = sizeof(content); content.node_count = 1; content.nodes = &node;
    content.node_stride = sizeof(node); content.reserved = 0;
    s->slots->set(s->binding, s->generation, &toolbar, &content);
    s->slots->set_handler(s->binding, s->generation, on_slot);
    if (s->store->get(s->binding, s->generation, launches_key, &s->launches, sizeof(s->launches), &got) != MC_OK)
        s->launches = 0;
    ++s->launches;
    s->store->set(s->binding, s->generation, launches_key, &s->launches, sizeof(s->launches));
    timer->set_handler(s->binding, s->generation, on_timer);
    if (timer->start(s->binding, s->generation, 1, 250, 1) != MC_OK) return MC_E_INVALID;
    return MC_OK;
}

static void MC_CALL stop(void* state, uint32_t state_bytes) {
    (void)state; (void)state_bytes;
}

MC_EXPORT mc_status MC_CALL maicord_plugin_query_v5(const mc_host_info* host, mc_plugin_definition* definition) {
    mc_plugin_definition local;
    char* raw = (char*)&local;
    uint32_t i;
    if (!MC_HAS_FIELD(host, mc_host_info, abi_version) || host->abi_version < 5) return MC_E_UNSUPPORTED;
    for (i = 0; i < sizeof(local); ++i) raw[i] = 0;
    local.size = sizeof(local);
    local.abi_version = MC_ABI_VERSION;
    local.owner_id = OWNER;
    local.permissions = MC_PERM_ALL;
    local.state_bytes = sizeof(fixture_state);
    local.state_alignment = 8;
    local.min_host_abi = 5;
    local.start = start;
    local.stop = stop;
    return mc_definition_publish(definition, &local);
}

/* Lets the test read the plugin's own counters through its state pointer. */
MC_EXPORT uint32_t maicord_plugin_sdk_full_counter(const void* state, uint32_t which) {
    const fixture_state* s = (const fixture_state*)state;
    switch (which) {
    case 0: return s->observed;
    case 1: return s->timer_fires;
    case 2: return s->slot_actions;
    case 3: return s->launches;
    case 4: return s->toggle;
    case 5: return (uint32_t)s->slider;
    case 6: return s->colour;
    case 7: return s->menu_presses;
    case 8: return s->notice_presses;
    case 9: return s->keybind_bytes;
    case 11: return s->popover_presses;
    default: return s->text_bytes;
    }
}

int MC_CALL maicord_plugin_sdk_full_entry(void* instance, unsigned long reason, void* reserved) {
    (void)instance; (void)reason; (void)reserved;
    return 1;
}

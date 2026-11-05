/* An ABI-6 plugin that decorates every surface and declares a button for each, for the surface tests and the UI
   probe. What it decorates depends on the message content's first word (see decorate_message), on the user's id
   and surface, the channel's unread flag, and so on. Its state (plugin_surfaces_fixture.h) records every call. */
#include "maicord/sdk/maicord_plugin.h"
#include "plugin_surfaces_fixture.h"

#include "plugin_sleep.h"

static mc_str str(const char* text) {
    mc_str result = {text, 0, 0};
    while (text[result.bytes]) ++result.bytes;
    return result;
}
static int begins(mc_str text, const char* word) {
    for (uint32_t i = 0; word[i]; ++i)
        if (i >= text.bytes || text.data[i] != word[i]) return 0;
    return 1;
}
static void set_node(mc_node* node, uint32_t kind, const char* text, const char* icon, uint32_t tone, const char* tip,
                     uint64_t tag) {
    node->kind = kind;
    node->tone = tone;
    node->text = str(text);
    node->icon = str(icon);
    node->tooltip = str(tip);
    node->tag = tag;
}

#if defined(SURFACES_SECOND)
/* A second decorating plugin, to show how verdicts merge: only messages, and only "affix" and "collapse". */
static mc_status MC_CALL decorate_message(mc_ctx* ctx, const mc_message* message, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->message_calls;
    if (begins(message->content, "affix")) {
        d->name_prefix = str("[Q]");
        d->name_suffix = str("(T)");
        d->name_tone = MC_TONE_WARNING;
        d->badge_count = 1;
        set_node(&d->badges[0], MC_NODE_BADGE, "C", "", MC_TONE_WARNING, "", 0);
    } else if (begins(message->content, "collapse")) {
        d->visibility = MC_VIS_COLLAPSED;
        d->collapsed_label = str("Other");
    }
    return MC_OK;
}
static const mc_hook hooks[] = {{MC_HOOK_DECORATE_MESSAGE, MC_NEED_CONTENT, 0, {.decorate_message = decorate_message}}};
MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    .size = sizeof(mc_descriptor),
    .abi = MC_ABI,
    .api_level = 1,
    .owner_id = SURFACES_OWNER + 1,
    .name = MC_S("Surfaces fixture two"),
    .version = MC_S("1.0.0"),
    .summary = MC_S("Decorates messages after the first fixture."),
    .author = MC_S("MaiCord"),
    .state_bytes = sizeof(surfaces_state),
    .state_align = 8,
    .state_layout = 1,
    .hook_count = 1,
    .hooks = hooks,
};
#else

static mc_status MC_CALL decorate_message(mc_ctx* ctx, const mc_message* message, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->message_calls;
    if (begins(message->content, "fail")) return MC_E_INVALID;
    if (begins(message->content, "slow")) Sleep(30);
    if (begins(message->content, "hidemedia")) {
        d->media = MC_MEDIA_HIDE;
    } else if (begins(message->content, "hide")) {
        d->visibility = MC_VIS_HIDDEN;
    } else if (begins(message->content, "collapse")) {
        d->visibility = MC_VIS_COLLAPSED;
        d->collapsed_label = str("Spoiler");
    } else if (begins(message->content, "blur")) {
        d->media = MC_MEDIA_BLUR;
    } else if (begins(message->content, "affix")) {
        d->name_prefix = str("[P]");
        d->name_suffix = str("(S)");
        d->name_tone = MC_TONE_ROLE;
        d->badge_count = 1;
        set_node(&d->badges[0], MC_NODE_BADGE, "B", "lucide/star", MC_TONE_ACCENT, "Badge tip", 0);
    } else if (begins(message->content, "rows")) {
        d->accessory_count = 3;
        set_node(&d->accessories[0], MC_NODE_LABEL, "Row one", "", MC_TONE_NEUTRAL, "", 0);
        set_node(&d->accessories[1], MC_NODE_BUTTON, "Do it", "", MC_TONE_ACCENT, "", 21);
        set_node(&d->accessories[2], MC_NODE_LINK, "Docs", "", MC_TONE_NEUTRAL, "", 0);
        d->accessories[2].value = str("https://example.com/docs");
    } else if (begins(message->content, "card")) {
        d->accessory_count = 1;
        set_node(&d->accessories[0], MC_NODE_LABEL, "Note", "", MC_TONE_NEUTRAL, "", 0);
        d->has_card = 1;
        d->card.title = str("Card title");
        d->card.text = str("Card text line one\nline two");
        d->card.caption = str("a caption");
        d->card.link = str("https://example.com/card");
        d->card.button_count = 2;
        d->card.buttons[0].label = str("Open");
        d->card.buttons[0].tag = 11;
        d->card.buttons[1].label = str("Dismiss");
        d->card.buttons[1].tag = 12;
        d->card.buttons[1].tone = MC_TONE_CRITICAL;
        d->card.tone = MC_TONE_ACCENT;
    } else if (begins(message->content, "replace")) {
        d->flags = MC_DECO_REPLACE_CONTENT;
        d->content = str("**Replaced** text");
    } else if (begins(message->content, "many")) {
        d->badge_count = MC_DECORATION_BADGES;
        for (uint32_t i = 0; i < MC_DECORATION_BADGES; ++i)
            set_node(&d->badges[i], MC_NODE_BADGE, "M", "", MC_TONE_NEUTRAL, "", 0);
        d->accessory_count = MC_DECORATION_ACCESSORIES;
        for (uint32_t i = 0; i < MC_DECORATION_ACCESSORIES; ++i)
            set_node(&d->accessories[i], MC_NODE_LABEL, "Many", "", MC_TONE_NEUTRAL, "", 0);
    } else if (begins(message->content, "bad")) {
        d->accessory_count = 1; /* a button without a tag: the whole answer is ignored */
        set_node(&d->accessories[0], MC_NODE_BUTTON, "Bad", "", MC_TONE_NEUTRAL, "", 0);
        d->visibility = MC_VIS_HIDDEN;
    }
    return MC_OK;
}

static void number(char* out, uint32_t value, uint32_t* at) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) out[(*at)++] = digits[--count];
}

static char g_online[24];
static mc_status MC_CALL decorate_header(mc_ctx* ctx, const mc_header* header, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->header_calls;
    d->badge_count = 1;
    if (header->where == MC_HEADER_CHANNEL) {
        set_node(&d->badges[0], MC_NODE_BADGE, "Fixture header", "lucide/info", MC_TONE_ACCENT, "", 0);
    } else {
        uint32_t at = 0;
        number(g_online, header->online_count, &at);
        const char tail[] = " online";
        for (uint32_t i = 0; tail[i]; ++i) g_online[at++] = tail[i];
        g_online[at] = 0;
        set_node(&d->badges[0], MC_NODE_LABEL, g_online, "", MC_TONE_NEUTRAL, "", 0);
    }
    return MC_OK;
}

static mc_status MC_CALL decorate_user(mc_ctx* ctx, const mc_user* user, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->user_calls;
    state->last_user_presence = user->presence;
    state->last_user_platforms = user->platforms;
    if (user->where == MC_USER_MEMBER_ROW) {
        if (user->user_id & 1) {
            d->badge_count = 1;
            set_node(&d->badges[0], MC_NODE_BADGE, "Fan", "lucide/star", MC_TONE_ACCENT, "", 0);
        }
    } else if (user->where == MC_USER_PROFILE) {
        d->accessory_count = 2;
        set_node(&d->accessories[0], MC_NODE_LABEL, "Profile row", "", MC_TONE_NEUTRAL, "", 0);
        set_node(&d->accessories[1], MC_NODE_BUTTON, "Poke", "", MC_TONE_ACCENT, "", 31);
    }
    return MC_OK;
}

static mc_status MC_CALL decorate_channel(mc_ctx* ctx, const mc_channel* channel, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->channel_calls;
    if (channel->flags & MC_CHANNEL_UNREAD) {
        d->badge_count = 1;
        set_node(&d->badges[0], MC_NODE_BADGE, "new", "", MC_TONE_POSITIVE, "", 0);
    }
    return MC_OK;
}

static mc_status MC_CALL decorate_guild(mc_ctx* ctx, const mc_guild* guild, mc_decoration* d) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->guild_calls;
    if (guild->flags & MC_GUILD_UNREAD) {
        d->badge_count = 1;
        set_node(&d->badges[0], MC_NODE_ICON, "", "lucide/bell", MC_TONE_ACCENT, "Unread", 0);
    }
    return MC_OK;
}

static mc_status MC_CALL node_action(mc_ctx* ctx, const mc_node_action* action) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->action_calls;
    state->last_action_tag = action->tag;
    state->last_action_message = action->message_id;
    state->last_action_user = action->user_id;
    state->last_action_channel = action->channel_id;
    state->last_action_kind = action->kind;
    state->last_action_source = action->source;
    state->last_action_flags = ctx->flags;
    return MC_OK;
}

/* ---- Buttons ---- */

static uint32_t MC_CALL visible_a(mc_ctx* ctx, const mc_button_event* event) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->visible_calls;
    return event->message && begins(event->message->content, "a");
}
static uint32_t MC_CALL visible_c(mc_ctx* ctx, const mc_button_event* event) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->visible_calls;
    return event->message && begins(event->message->content, "c");
}
static uint32_t MC_CALL visible_never(mc_ctx* ctx, const mc_button_event* event) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    (void)event;
    ++state->visible_calls;
    return 0;
}

static mc_status MC_CALL clicked(mc_ctx* ctx, const mc_button_event* event) {
    surfaces_state* state = (surfaces_state*)ctx->state;
    ++state->click_calls;
    state->last_button = event->button;
    state->last_click_channel = event->channel_id;
    state->last_click_guild = event->guild_id;
    state->last_click_message = event->message ? event->message->message_id : 0;
    state->last_click_content_bytes = event->message ? event->message->content.bytes : 0;
    state->last_click_flags = ctx->flags;
    if (event->button == SURFACES_BUTTON_CHAT_TOGGLE) {
        mc_button_state next = {0, 0, {0, 0, 0}, {0, 0, 0}};
        state->toggled ^= 1;
        if (state->toggled) {
            next.flags = MC_BUTTON_ACTIVE;
            next.badge_count = 3;
            next.tooltip = str("Fixture on");
        }
        state->state_status = ctx->api->button_state(ctx, SURFACES_BUTTON_CHAT_TOGGLE, &next);
    } else if (event->button == SURFACES_BUTTON_POPOVER_C && event->message) {
        mc_decoration pushed;
        char* bytes = (char*)&pushed;
        for (uint32_t i = 0; i < sizeof(pushed); ++i) bytes[i] = 0;
        pushed.accessory_count = 1;
        set_node(&pushed.accessories[0], MC_NODE_LABEL, "Pushed by click", "", MC_TONE_NEUTRAL, "", 0);
        state->push_status = ctx->api->decorate_push(ctx, MC_HOOK_DECORATE_MESSAGE, event->message->message_id,
                                                     event->message->channel_id, &pushed);
    }
    return MC_OK;
}

static mc_status MC_CALL start(mc_ctx* ctx) {
    /* A plugin may set a button's state before the host publishes its buttons. */
    surfaces_state* state = (surfaces_state*)ctx->state;
    mc_button_state first = {MC_BUTTON_DISABLED, 0, {0, 0, 0}, {0, 0, 0}};
    state->start_status = ctx->api->button_state(ctx, SURFACES_BUTTON_ACCOUNT, &first);
    first.flags = 0;
    ctx->api->button_state(ctx, SURFACES_BUTTON_ACCOUNT, &first);
    return MC_OK;
}

static const mc_button_def buttons[SURFACES_BUTTON_COUNT] = {
    {MC_SURFACE_CHAT_BAR, MC_BUTTON_TOGGLE, MC_S("Fixture toggle"), MC_S("lucide/languages"), 0, clicked},
    {MC_SURFACE_MESSAGE_POPOVER, 0, MC_S("Popover A"), MC_S("lucide/star"), visible_a, clicked},
    {MC_SURFACE_MESSAGE_POPOVER, 0, MC_S("Popover B"), MC_S("lucide/bug"), 0, clicked},
    {MC_SURFACE_MESSAGE_POPOVER, MC_BUTTON_DANGER, MC_S("Popover C"), MC_S("lucide/flag"), visible_c, clicked},
    {MC_SURFACE_ACCOUNT_PANEL, 0, MC_S("Fixture account"), MC_S("lucide/bell"), 0, clicked},
    {MC_SURFACE_HEADER, 0, MC_S("Fixture header button"), MC_S("lucide/info"), 0, clicked},
    {MC_SURFACE_SERVER_LIST_TOP, 0, MC_S("Fixture top"), MC_S("lucide/bell"), 0, clicked},
    {MC_SURFACE_SERVER_LIST_BOTTOM, 0, MC_S("Fixture bottom"), MC_S("lucide/compass"), 0, clicked},
    {MC_SURFACE_CHAT_BAR, 0, MC_S("Hidden by visible"), MC_S("lucide/ban"), visible_never, clicked},
};

static const mc_hook hooks[] = {
    {MC_HOOK_DECORATE_MESSAGE, MC_NEED_CONTENT, MC_FILTER_GUILD_ONLY, {.decorate_message = decorate_message}},
    {MC_HOOK_DECORATE_USER, 0, (1u << MC_USER_MEMBER_ROW) | (1u << MC_USER_PROFILE), {.decorate_user = decorate_user}},
    {MC_HOOK_DECORATE_CHANNEL, 0, 0, {.decorate_channel = decorate_channel}},
    {MC_HOOK_DECORATE_GUILD, 0, 0, {.decorate_guild = decorate_guild}},
    {MC_HOOK_DECORATE_HEADER, 0, 0, {.decorate_header = decorate_header}},
    {MC_HOOK_NODE_ACTION, 0, 0, {.node_action = node_action}},
};

MC_EXPORT const mc_descriptor MC_DESCRIPTOR_SYMBOL = {
    .size = sizeof(mc_descriptor),
    .abi = MC_ABI,
    .api_level = 1,
    .owner_id = SURFACES_OWNER,
    .name = MC_S("Surfaces fixture"),
    .version = MC_S("1.0.0"),
    .summary = MC_S("Decorates every surface and declares a button for each."),
    .author = MC_S("MaiCord"),
    .state_bytes = sizeof(surfaces_state),
    .state_align = 8,
    .state_layout = 1,
    .hook_count = sizeof(hooks) / sizeof(hooks[0]),
    .hooks = hooks,
    .buttons = buttons,
    .button_count = SURFACES_BUTTON_COUNT,
    .start = start,
};

#endif /* SURFACES_SECOND */

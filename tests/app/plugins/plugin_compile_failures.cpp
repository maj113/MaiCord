// The compile-time contract of maicord/sdk/plugin.hpp
// (tests/app/plugins/plugin_compile_failures.cmake): MC_CASE_VALID uses every
// authoring feature and must compile cleanly at /W4 /WX; each other case
// breaks one rule and must fail with its diagnostic.
#include "maicord/sdk/plugin.hpp"
using namespace mc;

#if defined(MC_CASE_SETTING_KEY)
MC_SETTINGS((Toggle, Loud, "Shout", false));
#elif defined(MC_CASE_CHOICE_DEFAULT)
MC_SETTINGS((Choice, size, "Size", 2, .items = {"Small", "Large"}));
#else
MC_SETTINGS((Header, general, "General"),
            (Toggle, loud, "Shout", false, .description = "Upper-case what you send"),
            (Int, repeat, "Repeat", 1, .min = 1, .max = 3),
            (Slider, volume, "Volume", 50, .min = 0, .max = 100, .step = 5, .visible_if = Setting::loud),
            (Choice, size, "Size", 1, .items = {"Small", "Large"}),
            (Text, signature, "Signature", "-- me", .hint = "Appended", .max_bytes = 64,
             .visible_if = Setting::size, .visible_value = 1),
            (Secret, token, "API key", {}, .hint = "Paste it here"),
            (TextList, words, "Words", {"a", "b"}, .flags = MC_SET_DANGER),
            (Colour, accent, "Accent", 0x5865F2),
            (Keybind, quick, "Quick reply", MC_KEY(0x26, MC_KEY_CTRL)),
            (Channel, log_channel, "Log to", {}),
            (UserList, ignored, "Ignore", {}));
#endif

struct State {
    u32 sent;
    u64 last_channel;
    u32 clicks;
};
#if defined(MC_CASE_STATE_NOT_TRIVIAL)
struct Owning {
    Owning() = default;
    Owning(const Owning&) {
    }
    u32 value;
};
MC_STATE(Owning);
#else
MC_STATE(State, .version = 2);
#endif

static void pre_send(Ctx& ctx, Draft& draft) {
    State& state = ctx.state<State>();
    ++state.sent;
    state.last_channel = draft.channel();
    const Settings& settings = ctx.settings();
    Text<256> out;
    for (u32 i = 0; i < static_cast<u32>(settings.repeat); ++i)
        out.append(draft.text());
    for (Str word : settings.words)
        out.append(word);
    if (settings.loud && settings.size == 1 && settings.volume > 0 && !settings.token.empty())
        out.append(settings.signature);
    if (settings.ignored.contains(draft.channel()) || settings.log_channel == draft.channel())
        draft.confirm("Send anyway?");
    else if (!draft.replace(out))
        draft.cancel();
    draft.set_flag(MC_DRAFT_SILENT, !draft.flag(MC_DRAFT_SILENT));
#if defined(MC_CASE_WRONG_STATE)
    ctx.state<u64>() = 1;
#endif
}
static void pre_edit(Ctx& ctx, EditDraft& draft) {
    if (draft.message() != 0)
        ctx.log(Level::debug, "editing");
}
static bool on_click(Ctx& ctx, const Message& m, const Click& click) {
    if (!click.double_click() || !m.own())
        return false;
    ctx.composer.edit(m.channel(), m.id());
    return true;
}
static bool keep_deleted(Ctx&, const DeletedMessage& m) {
    return !m.bot();
}
static bool keep_edit(Ctx&, const Message& before, const Message& after) {
    return before.content() != after.content();
}
static void on_created(Ctx& ctx, const ev::MessageCreate& e) {
    if (e.has(MC_EVENT_OWN))
        ctx.log(e.text());
}
static void on_unfriended(Ctx& ctx, const ev::RelationshipRemove& e) {
    ctx.toast(e.name(), Tone::warning);
}
static void on_lost(Ctx& ctx, const ev::Overflow&) {
    ctx.log(Level::warning, "events lost");
}
static void decorate(Ctx& ctx, const Message& m, Decoration& d) {
    if (m.content().empty())
        return;
    d.badge(badge("NEW", Tone::accent, "lucide/star", "Fresh"));
    d.accessory(button("Hide", m.id(), Tone::critical));
    d.accessory(link("Docs", "https://example.com"));
    d.card(card().title("Title").text("Body").caption("Caption").button("Open", 1).tone(Tone::positive));
    if (ctx.settings().loud)
        d.blur_media();
}
static void decorate_user(Ctx&, const User& u, Decoration& d) {
    if (u.bot())
        d.suffix(u.username(), Tone::neutral);
}
static void decorate_channel(Ctx&, const Channel& c, Decoration& d) {
    if (c.has(MC_CHANNEL_NSFW))
        d.badge(icon("lucide/flame", "Age-restricted"));
}
static void decorate_guild(Ctx&, const Guild& g, Decoration& d) {
    if (g.member_count > 1000)
        d.badge(badge("big"));
}
static void decorate_header(Ctx&, const Header& h, Decoration& d) {
    if (h.member_list())
        d.badge(label(h.name()));
}
static void on_node(Ctx& ctx, const NodeAction& action) {
    ctx.invalidate_message(action.message_id);
}
static bool on_typing(Ctx& ctx, const TypingNotice&) {
    return ctx.settings().loud;
}
static void on_setting(Ctx& ctx, const SettingChanged& changed) {
    if (changed.index == static_cast<u32>(Setting::loud))
        ctx.invalidate_buttons();
}
static void on_key(Ctx& ctx, const KeybindPressed&) {
    ctx.composer.focus();
}
static void on_timer(Ctx& ctx, const Timer& timer) {
    Activity activity{};
    activity.type = MC_ACTIVITY_LISTENING;
    activity.name = Str("Music");
    ctx.set_activity(timer.id ? &activity : nullptr);
}

static void on_fetched(Ctx& ctx, const HttpResult& r, u64 tag) {
    Text<64> value;
    if (r.ok() && json::find(r.body(), "value", value))
        ctx.composer.insert(value);
    ctx.invalidate_message(tag);
}
static void on_answer(Ctx& ctx, const ModalResult& r, u64) {
    if (r.accepted())
        ctx.send(ctx.settings().log_channel, r.text());
}
static void on_banner(Ctx& ctx, const BannerResult& r, u64) {
    if (r.action())
        ctx.open_url("https://example.com");
}

MC_COMMAND(shout, (Str text, Opt<i64> times, Opt<bool> loud), "Shout something", .args = {"What to shout"}) {
    Text<128> out;
    for (i64 i = 0; i < (times ? *times : 1); ++i)
        out.append(text);
    if (loud && *loud)
        ctx.toast("Loud!");
    return Reply::insert(out);
}
MC_COMMAND(ping, (), "Answer with pong", .guild_only = true) {
    return ctx.gesture() ? Reply::local("pong", Tone::positive) : Reply::none();
}
MC_COMMAND(who, (UserId user, Opt<ChannelId> where), "Show a user") {
    if (auto found = ctx.read.user(user.id))
        return Reply::toast(found->name());
    return where ? Reply::send("unknown") : Reply::none();
}

static bool has_url(Ctx&, const MenuTarget& t) {
    return !t.url().empty();
}
static bool is_loud(Ctx& ctx, const MenuTarget&) {
    return ctx.settings().loud;
}
MC_SUBMENU(tools, menu::link | menu::image, "Tools");
MC_MENU(copy_link, menu::link, "Copy link", .parent = tools, .visible = has_url) {
    ctx.copy(target.url());
}
MC_MENU(search, menu::image, "Search", .parent = tools) {
    Text<512> url;
    url.append("https://example.com/?q=");
    url::encode(target.url(), url);
    ctx.open_url(url);
}
MC_MENU(loudness, menu::message | menu::user, "Loud", .checked = is_loud) {
    ctx.set_setting(Setting::loud, !ctx.settings().loud);
}
MC_MENU(profile, menu::user, "Profile", .danger = true) {
    ctx.open_profile(target.user_id, target.guild_id);
}

static bool has_text(Ctx&, const ButtonEvent& b) {
    return !b.message().content().empty();
}
MC_BUTTON(fetch, surface::message_popover, "Fetch", "lucide/download", .visible = has_text) {
    ctx.http.get<on_fetched>("https://api.example.com/v1?x=1", button.message().id());
    ctx.http.post<on_fetched>("https://api.example.com/v1", "{}", 0,
                              {{"Content-Type", "application/json"}, {"Authorization", ctx.settings().token}});
}
MC_BUTTON(counter, surface::chat_bar, "Count", "lucide/hash", .toggle = true) {
    State& state = ctx.state<State>();
    ButtonState look{};
    look.flags = ++state.clicks % 2 ? MC_BUTTON_ACTIVE : 0;
    look.badge_count = state.clicks;
    ctx.button<counter>(look);
    Modal ask{};
    ask.title = Str("Message");
    ask.has_input = 1;
    ctx.modal<on_answer>(ask, 7);
}
MC_BUTTON(notice, surface::server_list_top, "Notice", "lucide/bell") {
    Banner b{};
    b.title = Str("Hello");
    b.action = Str("Open");
    ctx.banner<on_banner>(b);
    ctx.clear_banner();
    ctx.mark_read(MC_READ_ALL, 0);
    ctx.react(button.channel(), 1, "👍");
    ctx.delete_message(button.channel(), 1);
    ctx.edit_message(button.channel(), 1, "edited");
    ctx.timer(1, 1000, true);
    ctx.cancel_timer(1);
    const u64 id = ctx.local_message("Only you see this");
    Decoration pushed{};
    pushed.card(card().text("Pushed"));
    ctx.push_message(button.channel(), id, &pushed);
    ctx.push_user(1, nullptr);
    ctx.push_header(MC_HEADER_MEMBER_LIST, &pushed);
    ctx.remove_local_message(button.channel(), id);
    Text<64> draft;
    if (ctx.composer.text(draft))
        ctx.composer.replace(draft);
    ctx.composer.reply(button.channel(), 1);
    if (auto kept = ctx.kept.info(button.channel(), 1))
        ctx.kept.text(button.channel(), 1, kept->edit_count, draft);
    if (auto channel = ctx.read.channel(button.channel()))
        ctx.open_channel(channel->id());
    if (ctx.read.permissions(button.guild(), 0, 1) && ctx.read.guild(button.guild()))
        ctx.kept.restore(nullptr, 0);
    mc_message_ref recent[4];
    u64 groups[2];
    if (auto open = ctx.read.open_channel(); open && ctx.read.messages(open->channel_id, 0, recent))
        ctx.composer.clear_reply(open->channel_id);
    if (ctx.read.shared_group_dms(1, groups) && ctx.read.voice_channel(1) && ctx.now_ms() > 0 && ctx.local_offset(0))
        ctx.log("offset");
}

static void start(Ctx& ctx) {
    u32 count = 0;
    if (!ctx.store.get("count", count) && ctx.first_start())
        ctx.toast("Welcome", Tone::accent);
    ctx.store.put("count", count + 1);
    ctx.store.put("note", "text");
    Text<64> note;
    ctx.store.get("note", note);
    ctx.store.remove("old");
    ctx.set_setting(Setting::loud, true);
    ctx.set_setting(Setting::repeat, 2);
    ctx.set_setting(Setting::size, 0u);
    ctx.set_setting(Setting::signature, Str("--"));
    ctx.set_setting(Setting::log_channel, u64{1});
    ctx.log(note);
}
static void stop(Ctx& ctx) {
    if (ctx.gesture() || ctx.reloaded())
        ctx.log("stopping");
}

#if defined(MC_CASE_DUPLICATE_HOOK)
static void pre_send_again(Ctx&, Draft&) {
}
MC_HOOKS(pre_send, pre_edit, pre_send_again);
#elif defined(MC_CASE_UNKNOWN_HOOK)
static void on_number(Ctx&, int) {
}
MC_HOOKS(pre_send, pre_edit, on_number);
#elif defined(MC_CASE_DUPLICATE_EVENT)
static void on_created_again(Ctx&, const ev::MessageCreate&) {
}
MC_HOOKS(pre_send, on_created, on_created_again);
#elif defined(MC_CASE_FILTER_KIND)
MC_HOOKS(mc::filter<pre_send, 1>);
#else
MC_HOOKS(pre_send, pre_edit, mc::filter<on_click, MC_CLICK_FILTER_DOUBLE>, keep_deleted, keep_edit, on_created,
         on_unfriended, on_lost, mc::filter<decorate, MC_FILTER_GUILD_ONLY | MC_FILTER_NOT_OWN>,
         mc::filter<decorate_user, 1u << MC_USER_MEMBER_ROW>, decorate_channel, decorate_guild, decorate_header,
         on_node, on_typing, on_setting, on_key, on_timer);
#endif

#if defined(MC_CASE_NO_OWNER)
MC_PLUGIN(.name = "Contract", .version = "1.0.0");
#elif defined(MC_CASE_MISSING_PERMISSION)
MC_PLUGIN(.owner = 0x6D63'636F'6E74'7261, .name = "Contract", .version = "1.0.0",
          .permissions = MC_PERM_COMPOSER | MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_PERSISTENT_LOG |
                         MC_PERM_READ_MESSAGES,
          .network = {"api.example.com"});
#elif defined(MC_CASE_NO_NETWORK)
MC_PLUGIN(.owner = 0x6D63'636F'6E74'7261, .name = "Contract", .version = "1.0.0",
          .permissions = MC_PERM_COMPOSER | MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_PERSISTENT_LOG |
                         MC_PERM_CLIPBOARD | MC_PERM_READ_MESSAGES);
#else
MC_PLUGIN(.owner = 0x6D63'636F'6E74'7261, .name = "Contract", .version = "1.0.0",
          .summary = "Every feature.", .author = "MaiCord", .homepage = "https://example.com",
          .permissions = MC_PERM_COMPOSER | MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_PERSISTENT_LOG |
                         MC_PERM_CLIPBOARD | MC_PERM_READ_MESSAGES,
          .network = {"api.example.com", "*.example.org"}, .start = start, .stop = stop);
#endif

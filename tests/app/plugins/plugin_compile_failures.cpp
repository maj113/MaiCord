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
MC_SETTINGS((Toggle, loud, "Shout", false, .description = "Upper-case what you send"),
            (Int, repeat, "Repeat", 1, .min = 1, .max = 3),
            (Slider, volume, "Volume", 50, .min = 0, .max = 100, .step = 5),
            (Choice, size, "Size", 1, .items = {"Small", "Large"}),
            (Text, signature, "Signature", "-- me", .hint = "Appended", .max_bytes = 64),
            (TextList, words, "Words", {"a", "b"}, .flags = MC_SET_DANGER));
#endif

struct State {
    u32 sent;
    u64 last_channel;
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
    if (settings.loud && settings.size == 1 && settings.volume > 0)
        out.append(settings.signature);
    if (!draft.replace(out))
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
#else
MC_HOOKS(pre_send, pre_edit);
#endif

#if defined(MC_CASE_NO_OWNER)
MC_PLUGIN(.name = "Contract", .version = "1.0.0");
#else
MC_PLUGIN(.owner = 0x6D63'636F'6E74'7261, .name = "Contract", .version = "1.0.0",
          .summary = "Every feature.", .author = "MaiCord", .homepage = "https://example.com",
          .start = start, .stop = stop);
#endif

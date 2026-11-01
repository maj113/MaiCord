// Silent Toggle: a chat-bar toggle and a /silent command that send your
// messages as @silent (no push notifications) until you turn it off.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Toggle, silent, "Send messages silently", false, .flags = MC_SET_HIDDEN),
            (Toggle, once, "Turn off after one message", false));

static void show(Ctx& ctx, bool on);

MC_BUTTON(bell, surface::chat_bar, "Silent messages", "lucide/bell-off", .toggle = true) {
    show(ctx, !ctx.settings().silent);
}

static void show(Ctx& ctx, bool on) {
    ctx.set_setting(Setting::silent, on);
    ButtonState look{};
    look.flags = on ? MC_BUTTON_ACTIVE : 0;
    look.tooltip = on ? Str("Silent messages: on") : Str("Silent messages: off");
    ctx.button<bell>(look);
}

MC_COMMAND(silent, (Opt<bool> on), "Send messages without notifications", .args = {"On or off; omit to switch"}) {
    const bool next = on ? *on : !ctx.settings().silent;
    show(ctx, next);
    return Reply::toast(next ? Str("Messages are sent silently") : Str("Messages notify again"));
}

static void pre_send(Ctx& ctx, Draft& draft) {
    if (!ctx.settings().silent)
        return;
    draft.set_flag(MC_DRAFT_SILENT, true);
    if (ctx.settings().once)
        show(ctx, false);
}
static void start(Ctx& ctx) {
    show(ctx, ctx.settings().silent); // the button shows the saved state
}

MC_HOOKS(pre_send);
MC_PLUGIN(.owner = 0x6D63'736C'6E74'6F67, .name = "Silent Toggle", .version = "1.0.0",
          .summary = "Sends your messages as @silent while it's on.", .start = start);

// Clear URLs: removes tracking parameters from links in messages you send or edit.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Toggle, on_edit, "Also clean edited messages", true),
            (TextList, extra, "Extra parameters to remove", {}, .hint = "One per line, e.g. si"));

static void clean(Ctx& ctx, Draft& draft) {
    Text<MC_DRAFT_TEXT_MAX> out;
    if (url::strip_params(draft.text(), url::kTrackers, ctx.settings().extra, out))
        draft.replace(out);
}
static void pre_send(Ctx& ctx, Draft& draft) {
    clean(ctx, draft);
}
static void pre_edit(Ctx& ctx, EditDraft& draft) {
    if (ctx.settings().on_edit)
        clean(ctx, draft);
}

MC_HOOKS(pre_send, pre_edit); // kinds come from Draft& / EditDraft&; implies message_send
MC_PLUGIN(.owner = 0x6D63'636C'7572'6C73, .name = "Clear URLs", .version = "1.0.0",
          .summary = "Removes tracking parameters from links you send.");

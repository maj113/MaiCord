// The Developer Mode reload fixture: one source, four builds picked by RELOAD_VARIANT.
//   1 and 2 share a state layout and differ in their version, so 2 replaces 1 with the state kept;
//   3 changes the layout (a bigger state, version 2), so its state starts zeroed;
//   4 asks for one more permission, so a reload needs the user's say.
// The state counts what the host did to it; tests/app/plugins/plugin_dev.cpp reads it.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

#if !defined(RELOAD_VARIANT) || RELOAD_VARIANT < 1 || RELOAD_VARIANT > 4
#error "RELOAD_VARIANT is 1 to 4"
#endif

MC_SETTINGS((Toggle, mark, "Mark", false));

#if RELOAD_VARIANT == 3
struct State {
    u32 starts, started_as, calls, extra;
};
MC_STATE(State, .version = 2);
#define RELOAD_VERSION "2.0.0"
#elif RELOAD_VARIANT == 4
struct State {
    u32 starts, started_as, calls;
};
MC_STATE(State, .version = 1);
#define RELOAD_VERSION "2.1.0"
#elif RELOAD_VARIANT == 2
struct State {
    u32 starts, started_as, calls;
};
MC_STATE(State, .version = 1);
#define RELOAD_VERSION "1.0.1"
#else
struct State {
    u32 starts, started_as, calls;
};
MC_STATE(State, .version = 1);
#define RELOAD_VERSION "1.0.0"
#endif

// started_as: 1 at a first start, 2 after a reload that kept the state.
static void start(Ctx& ctx) {
    State& state = ctx.state<State>();
    ++state.starts;
    state.started_as = ctx.reloaded() ? 2 : 1;
    ctx.log("started");
}
static void stop(Ctx& ctx) {
    ctx.log("stopping");
}

static void pre_send(Ctx& ctx, Draft& draft) {
    ++ctx.state<State>().calls;
    if (draft.text() == Str("slow")) // longer than any test budget
        for (volatile u32 i = 0; i < 4'000'000; i = i + 1) {
        }
    if (ctx.settings().mark && draft.text() == Str("mark"))
        draft.replace("marked");
}
MC_HOOKS(pre_send);

MC_PLUGIN(.owner = 0x6D63'7265'6C6F'6164, .name = "Reload Fixture", .version = RELOAD_VERSION,
          .permissions = RELOAD_VARIANT == 4 ? MC_PERM_CLIPBOARD : 0, .start = start, .stop = stop);

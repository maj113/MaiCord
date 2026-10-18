#include "application_updates.hpp"
#include "application_settings.hpp"
#include "maicord/app/update_https.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/random.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kFirstCheckDelaySeconds = 60, kFirstCheckJitterSeconds = 120;
constexpr u64 kProgressIntervalMs = 250;
constexpr Utf8View channel_names[]{utf8_literal("Stable"), utf8_literal("Preview")};
using State = ApplicationUpdateState;

struct Worker {
    UpdateService service;
    UpdateHttpsTransport https;
    UpdateServiceConfig config;
    UpdatePaths paths;
    SrwLock lock;
    HANDLE thread, wake, stop, notify;
    ApplicationUpdatesView view; // guarded by lock
    u64 revision;                // guarded by lock
    u64 progress_at_ms;
    u32 failures;
    bool check_requested, restart_requested, revert_requested, channel_requested, quit;
};
Worker* g_worker;
ApplicationUpdatesView g_view;
ApplicationUpdatesText g_text[4];
u64 g_seen_revision;
bool g_quit_reported;

bool busy_state(State state) noexcept {
    return state == State::checking || state == State::downloading || state == State::installing ||
           state == State::restarting;
}
u64 now_unix() noexcept { return clock_unix_milliseconds() / 1000; }
u32 random32() noexcept { u32 value = 0; secure_random_u32(&value); return value; }
void publish(Worker* w, void (*change)(Worker*, void*), void* context) noexcept {
    AcquireSRWLockExclusive(w->lock.native());
    change(w, context);
    ++w->revision;
    ReleaseSRWLockExclusive(w->lock.native());
    SetEvent(w->notify);
}
void set_state(Worker* w, State state, UpdateServiceError error = UpdateServiceError::none) noexcept {
    struct C { State s; UpdateServiceError e; } c{state, error};
    publish(w, [](Worker* worker, void* p) noexcept {
        const auto& c = *static_cast<C*>(p);
        worker->view.state = c.s; worker->view.error = c.e;
        if (c.s != State::downloading && c.s != State::installing) worker->view.received = worker->view.total = 0;
    }, &c);
}
// Ready/rollback availability from the installed journal.
void refresh_installed(Worker* w) noexcept {
    UpdateInstalledState installed{};
    if (update_service_inspect(&w->service, &installed) != UpdateServiceError::none) return;
    struct C { UpdateInstalledState i; u64 own; } c{installed, w->config.own_version};
    publish(w, [](Worker* worker, void* p) noexcept {
        const auto& c = *static_cast<C*>(p);
        auto& v = worker->view;
        const bool stable = c.i.phase == UpdateGenerationPhase::stable;
        v.rollback_version = c.i.journal && stable && c.i.previous_version && c.i.previous_version < c.i.active_version &&
            c.i.active_version == c.own ? c.i.previous_version : 0;
        u64 ready = 0;
        if (c.i.journal && c.i.phase == UpdateGenerationPhase::prepared) ready = c.i.candidate_version;
        else if (c.i.journal && stable && c.i.active_version > c.own) ready = c.i.active_version;
        if (ready) { v.ready_version = ready; if (v.state == State::idle || v.state == State::up_to_date) v.state = State::ready; }
    }, &c);
}
// Wraps the HTTPS transport to report download progress at a bounded rate.
struct Relay { Worker* worker; UpdateBodySink sink; void* context; };
bool relay_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* relay = static_cast<Relay*>(context);
    if (!relay->sink(relay->context, bytes)) return false;
    Worker* w = relay->worker;
    const u64 now = clock_monotonic_milliseconds();
    const LONG64 total = InterlockedCompareExchange64(&w->service.progress.total, 0, 0);
    const LONG64 received = InterlockedCompareExchange64(&w->service.progress.received, 0, 0);
    // The last bytes switch to "installing" at once: staging and the
    // generation install follow, and a stale percentage would read as a stall.
    const bool done = total && received >= total;
    if (total && (done || now - w->progress_at_ms >= kProgressIntervalMs)) {
        w->progress_at_ms = now;
        struct C { u64 r, t; bool done; } c{static_cast<u64>(received), static_cast<u64>(total), done};
        publish(w, [](Worker* worker, void* p) noexcept {
            const auto& c = *static_cast<C*>(p);
            worker->view.state = c.done ? State::installing : State::downloading; worker->view.received = c.r; worker->view.total = c.t;
        }, &c);
    }
    return true;
}
UpdateTransportError relay_get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    auto* w = static_cast<Worker*>(context);
    Relay relay{w, request.sink, request.sink_context};
    UpdateHttpRequest wrapped = request; wrapped.sink = relay_sink; wrapped.sink_context = &relay;
    const auto inner = update_https_transport(&w->https);
    return inner.get(inner.context, wrapped, response);
}
// Returns the server-requested earliest retry, if any.
u64 run_check(Worker* w, UpdateChannel channel) noexcept {
    set_state(w, State::checking);
    InterlockedExchange64(&w->service.progress.total, 0);
    const auto result = update_service_check(&w->service, {w, relay_get}, channel, now_unix());
    if (result.outcome == UpdateCheckOutcome::failed) {
        ++w->failures;
        set_state(w, result.error == UpdateServiceError::rate_limited ? State::rate_limited : State::error, result.error);
    } else {
        w->failures = 0;
        publish(w, [](Worker* worker, void* p) noexcept {
            const auto& r = *static_cast<const UpdateCheckResult*>(p);
            auto& v = worker->view;
            v.error = UpdateServiceError::none; v.received = v.total = 0;
            v.state = r.outcome == UpdateCheckOutcome::ready ? State::ready : State::up_to_date;
            if (r.outcome == UpdateCheckOutcome::ready) v.ready_version = r.version;
        }, const_cast<UpdateCheckResult*>(&result));
    }
    refresh_installed(w);
    return result.retry_at_unix;
}
DWORD WINAPI worker_main(void* parameter) noexcept {
    auto* w = static_cast<Worker*>(parameter);
    refresh_installed(w);
    u64 next = now_unix() + kFirstCheckDelaySeconds + random32() % kFirstCheckJitterSeconds;
    for (;;) {
        const u64 now = now_unix();
        const DWORD timeout = next <= now ? 0 : static_cast<DWORD>(minimum<u64>(next - now, 3600) * 1000);
        HANDLE handles[]{w->stop, w->wake};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
        AcquireSRWLockExclusive(w->lock.native());
        const bool check = w->check_requested, restart = w->restart_requested, revert = w->revert_requested;
        const bool channel_changed = w->channel_requested;
        const UpdateChannel channel = w->view.channel;
        w->check_requested = w->restart_requested = w->revert_requested = w->channel_requested = false;
        ReleaseSRWLockExclusive(w->lock.native());
        if (channel_changed) update_channel_save(w->paths, channel);
        if (revert || restart) {
            set_state(w, State::restarting);
            UpdateServiceError error = revert ? update_service_revert(&w->service) : UpdateServiceError::none;
            if (error == UpdateServiceError::none) error = update_service_restart(&w->service);
            if (error == UpdateServiceError::none) {
                publish(w, [](Worker* worker, void*) noexcept { worker->quit = true; }, nullptr);
                break;
            }
            set_state(w, State::error, error);
            refresh_installed(w);
            continue;
        }
        if (!check && !channel_changed && now_unix() < next) continue;
        const u64 retry_at = run_check(w, channel);
        next = update_next_check(now_unix(), w->failures, retry_at, random32());
    }
    return 0;
}
// Formats "0.2.0" etc. into a NUL-free view of out.
Utf8View version_text(u64 version, char* out, u32 capacity) noexcept {
    return {out, update_version_text(version, {out, capacity})};
}
struct Text : TextBank {
    void version(u64 v) noexcept { char t[32]; add(version_text(v, t, sizeof(t))); }
};
} // namespace

bool application_updates_project_view(const ApplicationUpdatesView& v, SettingBinding binding, ApplicationUpdatesText* text,
    UiSettingsControl* control, UiSettingsControlPresentation* presentation) noexcept {
    if (!text || !control || !presentation) return false;
    Text label{{text->label, sizeof(text->label), 0}};
    Text description{{text->description, sizeof(text->description), 0}};
    const bool disabled = v.state == State::disabled;
    const bool busy = busy_state(v.state);
    switch (binding) {
    case SettingBinding::update_status: {
        description.add(utf8_literal("MaiCord ")); description.version(v.own_version);
        description.add(disabled ? utf8_literal(".") : v.channel == UpdateChannel::preview ? utf8_literal(", preview channel.")
                                                                                            : utf8_literal(", stable channel."));
        control->action_label = utf8_literal("Check now");
        control->enabled = !busy && !disabled;
        switch (v.state) {
        case State::disabled:
            label.add(utf8_literal("Updates aren't configured for this build"));
            description.add(utf8_literal(" Download new versions manually."));
            control->kind = UiSettingsControlKind::notice; control->action_label = {};
            break;
        case State::idle: label.add(utf8_literal("Check for updates")); break;
        case State::checking: label.add(utf8_literal("Checking for updates…")); break;
        case State::downloading:
            label.add(utf8_literal("Downloading update"));
            if (v.total) { label.add(utf8_literal(" ")); label.number(minimum<u64>(100, v.received * 100 / v.total)); label.add(utf8_literal("%")); }
            break;
        case State::installing: label.add(utf8_literal("Installing update…")); break;
        case State::up_to_date: label.add(utf8_literal("You're up to date")); break;
        case State::ready:
            label.add(utf8_literal("Update ready"));
            description.used = 0;
            description.add(utf8_literal("Restart MaiCord to finish updating to ")); description.version(v.ready_version);
            description.add(utf8_literal("."));
            control->action_label = utf8_literal("Restart");
            presentation->variant = UiSettingsVariant::primary;
            break;
        case State::restarting: label.add(utf8_literal("Restarting…")); control->action_label = utf8_literal("Restart"); break;
        case State::error:
        case State::rate_limited:
            label.add(v.state == State::rate_limited ? utf8_literal("Couldn't check for updates") : utf8_literal("Couldn't update"));
            description.used = 0; description.add(update_service_error_text(v.error));
            control->action_label = utf8_literal("Try again");
            break;
        }
        control->label = label.view(); control->description = description.view();
        return true;
    }
    case SettingBinding::update_channel:
        control->selected_choice = static_cast<u32>(v.channel);
        control->choices = {channel_names, 2};
        control->enabled = !busy;
        return !disabled;
    case SettingBinding::update_rollback:
        if (disabled || !v.rollback_version) return false;
        description.add(utf8_literal("Go back to ")); description.version(v.rollback_version);
        description.add(utf8_literal(". Newer releases install again when they come out."));
        control->description = description.view();
        control->enabled = !busy;
        return true;
    case SettingBinding::update_notice:
        return !disabled;
    default:
        return false;
    }
}

bool application_updates_initialize(const UpdateServiceConfig& config) noexcept {
    if (g_worker) return true;
    auto* w = static_cast<Worker*>(VirtualAlloc(nullptr, sizeof(Worker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w) return false;
    w->config = config;
    InitializeSRWLock(w->lock.native());
    w->view.own_version = config.own_version;
    w->view.state = config.configured ? State::idle : State::disabled;
    const bool located = update_paths_default(&w->paths);
    w->view.channel = located ? update_channel_load(w->paths, config.default_channel) : config.default_channel;
    if (config.configured && !located) { w->view.state = State::error; w->view.error = UpdateServiceError::storage; }
    g_view = w->view;
    g_worker = w;
    UpdateLog("updater").field("configured", config.configured).field("located", located).field("own", config.own_version)
        .field("channel", static_cast<u8>(w->view.channel)).write();
    if (!config.configured || !located) return true;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->wake && w->stop && w->notify && update_service_initialize(&w->service, config, w->paths) == UpdateServiceError::none &&
        update_https_initialize(&w->https))
        w->thread = CreateThread(nullptr, 256 * 1024, worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!w->thread) { w->view.state = State::error; w->view.error = UpdateServiceError::storage; g_view = w->view; }
    return w->thread != nullptr;
}
void application_updates_release() noexcept {
    Worker* w = g_worker;
    if (!w) return;
    g_worker = nullptr;
    if (w->thread) {
        SetEvent(w->stop);
        update_https_cancel(&w->https);
        // A restart confirmation or journal commit in flight finishes or is
        // recovered cold; never free storage the worker may still touch.
        if (WaitForSingleObject(w->thread, 3000) != WAIT_OBJECT_0) return;
        CloseHandle(w->thread);
    }
    update_service_release(&w->service);
    update_https_release(&w->https);
    const HANDLE events[]{w->wake, w->stop, w->notify};
    for (HANDLE h : events) if (h) CloseHandle(h);
    VirtualFree(w, 0, MEM_RELEASE);
}
HANDLE application_updates_notification() noexcept { return g_worker ? g_worker->notify : nullptr; }
bool application_updates_poll() noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockShared(w->lock.native());
    const u64 revision = w->revision;
    const bool quit = w->quit;
    if (revision != g_seen_revision) g_view = w->view;
    ReleaseSRWLockShared(w->lock.native());
    if (revision != g_seen_revision) { g_seen_revision = revision; application_settings_refresh_pages(); }
    if (quit && !g_quit_reported) { g_quit_reported = true; return true; }
    return false;
}
bool application_updates_project(SettingBinding binding, UiSettingsControl* control, UiSettingsControlPresentation* presentation) noexcept {
    u32 slot = binding == SettingBinding::update_status ? 0 : binding == SettingBinding::update_channel ? 1
        : binding == SettingBinding::update_rollback ? 2 : 3;
    return application_updates_project_view(g_view, binding, &g_text[slot], control, presentation);
}
bool application_updates_activate(SettingBinding binding, u32 choice) noexcept {
    Worker* w = g_worker;
    if (!w || !w->thread) return false;
    AcquireSRWLockExclusive(w->lock.native());
    const auto state = w->view.state;
    bool accepted = !busy_state(state);
    if (accepted) {
        if (binding == SettingBinding::update_status) {
            if (state == State::ready) w->restart_requested = true; else w->check_requested = true;
        } else if (binding == SettingBinding::update_rollback && w->view.rollback_version) w->revert_requested = true;
        else if (binding == SettingBinding::update_channel && choice <= 1 && choice != static_cast<u32>(w->view.channel)) {
            w->view.channel = static_cast<UpdateChannel>(choice); w->channel_requested = true; ++w->revision;
        } else accepted = false;
    }
    ReleaseSRWLockExclusive(w->lock.native());
    if (accepted) { SetEvent(w->wake); SetEvent(w->notify); }
    return accepted;
}

} // namespace maicord::app_detail

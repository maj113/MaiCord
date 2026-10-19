#include "application_updates.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/app/update_https.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/random.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord::app_detail {
namespace {
// Fallback when the first frame is never presented.
constexpr u64 kFallbackCheckDelaySeconds = 60, kFallbackCheckJitterSeconds = 120;
constexpr u64 kProgressIntervalMs = 250;
constexpr Utf8View channel_names[]{utf8_literal("Stable"), utf8_literal("Preview")};
constexpr Utf8View interval_names[]{utf8_literal("Every hour"), utf8_literal("Every 4 hours"),
                                    utf8_literal("Every 12 hours"), utf8_literal("Once a day")};
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
    UpdateSchedule schedule; // worker thread
    u32 failures;
    bool check_requested, restart_requested, revert_requested, channel_requested, interval_requested, ready_requested, quit;
    UpdateChangelogDownload download; // worker thread
    UpdateChangelog published;        // guarded by lock
    ApplicationUpdatesChangelogRows rows; // UI thread
    ApplicationUpdatesSettingsBank bank;  // UI thread
};
Worker* g_worker;
ApplicationUpdatesView g_view;
ApplicationUpdatesText g_text[7];
u64 g_seen_revision;
bool g_quit_reported, g_ready_reported;

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
// Under the lock: forget the offer and its changelog.
void clear_offer(Worker* w) noexcept {
    auto& v = w->view;
    if (v.offer_version || w->published.version) ++v.changelog_revision;
    v.offer_version = v.offer_bytes = v.offer_date = 0;
    clear_bytes(v.offer_commit, sizeof(v.offer_commit));
    w->published.version = 0; w->published.count = 0; w->published.text_bytes = 0;
}
// Worker: a verified newer offer, before admission and download.
void offered(void* context, const UpdateManifest& manifest, const UpdateChangelog* changelog) noexcept {
    struct C { const UpdateManifest* m; const UpdateChangelog* log; } c{&manifest, changelog};
    publish(static_cast<Worker*>(context), [](Worker* worker, void* p) noexcept {
        const auto& c = *static_cast<C*>(p);
        auto& v = worker->view;
        auto& out = worker->published;
        clear_offer(worker);
        v.offer_version = c.m->version; v.offer_bytes = c.m->artifact_bytes;
        if (const auto* log = c.log) {
            out.version = log->version; out.base = log->base; out.channel = log->channel;
            out.count = log->count; out.text_bytes = log->text_bytes;
            copy_bytes(out.entries, log->entries, log->count * static_cast<u32>(sizeof(log->entries[0])));
            copy_bytes(out.text, log->text, log->text_bytes);
            for (u32 i = 0; i < log->count; ++i) {
                const auto& entry = log->entries[i];
                if (entry.version != log->version) continue;
                v.offer_date = entry.date_unix_seconds;
                if (entry.has_commit) copy_bytes(v.offer_commit, entry.commit, kApplicationUpdatesCommitChars);
                break;
            }
        }
        ++v.changelog_revision;
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
            else clear_offer(worker);
        }, const_cast<UpdateCheckResult*>(&result));
    }
    refresh_installed(w);
    return result.retry_at_unix;
}
UpdateCheckInterval current_interval(Worker* w) noexcept {
    AcquireSRWLockShared(w->lock.native());
    const UpdateCheckInterval interval = w->view.interval;
    ReleaseSRWLockShared(w->lock.native());
    return interval;
}
DWORD WINAPI worker_main(void* parameter) noexcept {
    auto* w = static_cast<Worker*>(parameter);
    refresh_installed(w);
    auto& schedule = w->schedule;
    schedule.next_unix = now_unix() + kFallbackCheckDelaySeconds + random32() % kFallbackCheckJitterSeconds;
    for (;;) {
        const u64 now = now_unix();
        const DWORD timeout = update_schedule_due(schedule, now) ? 0
            : static_cast<DWORD>(minimum<u64>(schedule.next_unix - now, 3600) * 1000);
        HANDLE handles[]{w->stop, w->wake};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
        AcquireSRWLockExclusive(w->lock.native());
        const bool check = w->check_requested, restart = w->restart_requested, revert = w->revert_requested;
        const bool channel_changed = w->channel_requested, interval_changed = w->interval_requested, ready = w->ready_requested;
        const UpdateChannel channel = w->view.channel;
        const UpdateCheckInterval interval = w->view.interval;
        w->check_requested = w->restart_requested = w->revert_requested = w->channel_requested = false;
        w->interval_requested = w->ready_requested = false;
        ReleaseSRWLockExclusive(w->lock.native());
        if (channel_changed) update_channel_save(w->paths, channel);
        if (ready && !schedule.checked_unix)
            schedule.next_unix = minimum(schedule.next_unix, update_first_check(now_unix(), random32()));
        if (interval_changed) {
            update_interval_save(w->paths, interval);
            update_schedule_retarget(&schedule, update_check_interval_seconds(interval), random32());
        }
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
        if (!check && !channel_changed && !update_schedule_due(schedule, now_unix())) continue;
        const u64 retry_at = run_check(w, channel);
        update_schedule_finish(&schedule, now_unix(), w->failures, retry_at, random32(),
            update_check_interval_seconds(current_interval(w)));
    }
    return 0;
}
Utf8View commit_of(const char (&commit)[kApplicationUpdatesCommitChars]) noexcept {
    return {commit, commit[0] ? kApplicationUpdatesCommitChars : 0u};
}
void add_build(TextBank& t, u64 version, Utf8View commit = {}) noexcept {
    char text[40];
    t.add({text, update_build_version_text(version, commit, {text, sizeof(text)})});
}
// 1024-based like Windows Explorer: "8.2 MB", "640 KB".
void add_size(TextBank& t, u64 bytes) noexcept {
    if (bytes >= 1024 * 1024) {
        const u64 tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
        t.number(tenths / 10); t.add(utf8_literal(".")); t.number(tenths % 10); t.add(utf8_literal(" MB"));
    } else {
        t.number((bytes + 1023) / 1024); t.add(utf8_literal(" KB"));
    }
}
void add_date(TextBank& t, u64 unix_seconds, bool year) noexcept {
    char text[24];
    t.add({text, update_changelog_date_text(unix_seconds, year, {text, sizeof(text)})});
}
// Only master builds carry their commit in the label.
Utf8View own_commit(const ApplicationUpdatesView& v) noexcept {
    return update_version_channel(v.own_version) == UpdateChannel::preview ? commit_of(v.own_commit) : Utf8View{};
}
u64 offered_version(const ApplicationUpdatesView& v) noexcept {
    const u64 version = v.offer_version > v.ready_version ? v.offer_version : v.ready_version;
    return version > v.own_version ? version : 0;
}
} // namespace

u32 application_updates_own_label(const ApplicationUpdatesView& v, Slice<char> output) noexcept {
    return update_build_version_text(v.own_version, own_commit(v), output);
}
u32 application_updates_offer_label(const ApplicationUpdatesView& v, Slice<char> output) noexcept {
    const u64 version = offered_version(v);
    // The commit names the offer only when the changelog described this version.
    return version ? update_build_version_text(version, version == v.offer_version ? commit_of(v.offer_commit) : Utf8View{}, output) : 0;
}

bool application_updates_project_view(const ApplicationUpdatesView& v, SettingBinding binding, ApplicationUpdatesText* text,
    UiSettingsControl* control, UiSettingsControlPresentation* presentation, u16* meter) noexcept {
    if (!text || !control || !presentation) return false;
    if (meter) *meter = kUiSettingsNoMeter;
    TextBank label{text->label, sizeof(text->label), 0};
    TextBank description{text->description, sizeof(text->description), 0};
    const bool disabled = v.state == State::disabled;
    const bool busy = busy_state(v.state);
    const bool failed = v.state == State::error || v.state == State::rate_limited;
    const u64 offer = offered_version(v);
    switch (binding) {
    case SettingBinding::update_status: {
        description.add(utf8_literal("MaiCord ")); add_build(description, v.own_version, own_commit(v));
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
            description.add(utf8_literal("Restart MaiCord to finish updating to ")); add_build(description, v.ready_version);
            description.add(utf8_literal("."));
            control->action_label = utf8_literal("Restart");
            presentation->variant = UiSettingsVariant::primary;
            break;
        case State::restarting: label.add(utf8_literal("Restarting…")); control->action_label = utf8_literal("Restart"); break;
        case State::error:
        case State::rate_limited:
            label.add(v.state == State::rate_limited ? utf8_literal("Couldn't check for updates") : utf8_literal("Couldn't update"));
            control->action_label = utf8_literal("Try again");
            break;
        }
        if (busy) {
            // Work in progress reads as a status notice with the loading spinner.
            control->kind = UiSettingsControlKind::notice; control->checked = true;
            control->action_label = {}; control->enabled = true;
        }
        control->label = label.view(); control->description = description.view();
        return true;
    }
    case SettingBinding::update_problem:
        if (!failed) return false;
        control->kind = UiSettingsControlKind::notice;
        control->label = {};
        control->description = description.add(update_service_error_text(v.error));
        control->action_label = {};
        presentation->variant = UiSettingsVariant::critical;
        return control->description.bytes != 0;
    case SettingBinding::update_available: {
        if (disabled || !offer) return false;
        label.add(utf8_literal("MaiCord "));
        char t[40];
        label.add({t, application_updates_offer_label(v, {t, sizeof(t)})});
        const bool preview = update_version_channel(offer) == UpdateChannel::preview;
        switch (v.state) {
        case State::downloading:
            description.add(utf8_literal("Downloading "));
            if (v.total) { add_size(description, v.received); description.add(utf8_literal(" of ")); add_size(description, v.total); }
            description.add(utf8_literal("…"));
            if (meter && v.total) *meter = static_cast<u16>(minimum<u64>(1000, v.received * 1000 / v.total));
            break;
        case State::installing:
            description.add(utf8_literal("Checking the download and installing it…"));
            if (meter) *meter = 1000;
            break;
        case State::ready:
        case State::restarting:
            description.add(utf8_literal("Downloaded and verified. Restart MaiCord to finish."));
            break;
        case State::error:
        case State::rate_limited:
            description.add(utf8_literal("Not installed yet. MaiCord tries again on the next check."));
            break;
        default:
            description.add(utf8_literal("Signed by the MaiCord publisher."));
            break;
        }
        TextBank chips{text->chip_text, sizeof(text->chip_text), 0};
        u32 count = 0;
        text->chips[count++] = chips.add(preview ? utf8_literal("Preview build") : utf8_literal("Stable release"));
        if (offer == v.offer_version && v.offer_date) {
            const u32 start = chips.used;
            chips.add(preview ? utf8_literal("Built ") : utf8_literal("Released "));
            add_date(chips, v.offer_date, true);
            text->chips[count++] = chips.since(start);
        }
        if (offer == v.offer_version && v.offer_bytes) {
            const u32 start = chips.used;
            add_size(chips, v.offer_bytes);
            text->chips[count++] = chips.since(start);
        }
        text->chips[count++] = chips.add(utf8_literal("Signature verified"));
        control->label = label.view(); control->description = description.view();
        control->choices = {text->chips, count};
        control->action_label = {};
        control->enabled = true;
        return true;
    }
    case SettingBinding::update_channel:
        control->selected_choice = static_cast<u32>(v.channel);
        control->choices = {channel_names, 2};
        control->enabled = !busy;
        if (v.channel == UpdateChannel::preview)
            description.add(utf8_literal("Every master build that passes CI, often several a day. New features first; "
                                         "expect rough edges."));
        else {
            description.add(utf8_literal("Tested releases. Recommended for most people."));
            if (update_version_channel(v.own_version) == UpdateChannel::preview)
                description.add(utf8_literal(" This preview build stays installed until a newer stable release comes out."));
        }
        control->description = description.view();
        return !disabled;
    case SettingBinding::update_interval:
        control->selected_choice = static_cast<u32>(v.interval);
        control->choices = {interval_names, kUpdateCheckIntervalCount};
        control->enabled = true;
        return !disabled;
    case SettingBinding::update_rollback:
        if (disabled || !v.rollback_version) return false;
        description.add(utf8_literal("Go back to ")); add_build(description, v.rollback_version);
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

void application_updates_changelog_build(const ApplicationUpdatesView& v, const UpdateChangelog& log,
    ApplicationUpdatesChangelogRows* out) noexcept {
    if (!out) return;
    out->count = 0; out->heading = {}; out->built = true;
    out->built_revision = v.changelog_revision; out->built_own = v.own_version; out->built_offer = v.offer_version;
    out->bank = {out->text, sizeof(out->text), 0};
    out->preview = log.channel == UpdateChannel::preview;
    if (!log.version || log.version != v.offer_version || log.version <= v.own_version) return;
    TextBank& bank = out->bank;
    {
        const u32 start = bank.used;
        bank.add(utf8_literal("What's new since "));
        add_build(bank, v.own_version, own_commit(v));
        out->heading = bank.since(start);
    }
    u32 newer = 0;
    for (u32 i = 0; i < log.count; ++i) newer += log.entries[i].version > v.own_version;
    const u32 room = kApplicationUpdatesChangelogRows - 1;
    u32 shown = 0;
    for (u32 i = 0; i < log.count && out->count < room; ++i) {
        const auto& entry = log.entries[i];
        if (entry.version <= v.own_version) continue;
        auto& row = out->rows[out->count++];
        row = {};
        row.commit = out->preview;
        if (out->preview) {
            const auto source = update_changelog_text(log, entry);
            u32 bytes = 0;
            while (bytes < source.bytes && source.data[bytes] != '\n') ++bytes;
            if (!bytes) { --out->count; continue; }
            row.label = bank.add({source.data, bytes});
            const u32 start = bank.used;
            add_date(bank, entry.date_unix_seconds, false);
            row.column[0] = bank.since(start);
        } else {
            const u32 start = bank.used;
            bank.add(utf8_literal("MaiCord ")); add_build(bank, entry.version);
            if (entry.date_unix_seconds) { bank.add(utf8_literal(" · ")); add_date(bank, entry.date_unix_seconds, true); }
            row.label = bank.since(start);
            const u32 notes = bank.used;
            bank.used += update_changelog_display_text(update_changelog_text(log, entry),
                {bank.data + bank.used, bank.capacity - bank.used});
            row.description = bank.since(notes);
        }
        ++shown;
    }
    // Truncated lists and changelogs that start after this build say so.
    const u32 start = bank.used;
    if (shown < newer) {
        bank.number(newer - shown);
        bank.add(out->preview ? utf8_literal(" earlier changes aren't shown.") : utf8_literal(" earlier releases aren't shown."));
    } else if (v.own_version < log.base) {
        bank.add(utf8_literal("Changes up to "));
        add_build(bank, log.base);
        bank.add(utf8_literal(" aren't listed here."));
    }
    if (bank.used != start) out->rows[out->count++] = {{}, bank.since(start), {}, false};
}

bool application_updates_changelog_heading(const ApplicationUpdatesChangelogRows& rows, UiSettingsControl* control,
    UiSettingsListItem* item) noexcept {
    static const Utf8View date_column[]{utf8_literal("Date")};
    if (!rows.built || !rows.heading.bytes || !rows.count || !control) return false;
    control->label = rows.heading;
    control->description = {};
    control->kind = UiSettingsControlKind::category;
    if (rows.preview) control->choices = {date_column, 1};
    if (item)
        *item = rows.preview ? UiSettingsListItem{UiSettingsListStyle::header, UiIcon::count, UiIcon::count,
                                                  {UiIcon::count, UiIcon::count}, false}
                             : UiSettingsListItem{};
    return true;
}

u32 application_updates_changelog_project(const ApplicationUpdatesChangelogRows& rows, Slice<UiSettingsControl> controls,
    Slice<UiSettingsControlPresentation> presentation) noexcept {
    if (!rows.built || !rows.heading.bytes) return 0;
    u32 count = 0;
    for (u32 i = 0; i < rows.count && count < controls.count && count < presentation.count; ++i) {
        const auto& row = rows.rows[i];
        auto& control = controls[count];
        control = {settings_list_control_id(SettingBinding::update_changelog, i), row.label, row.description,
                   UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
        presentation[count] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
        if (row.commit) {
            control.kind = UiSettingsControlKind::action;
            control.choices = {row.column, row.column[0].bytes ? 1u : 0u};
        }
        ++count;
    }
    return count;
}
UiSettingsListItem application_updates_changelog_item(const ApplicationUpdatesChangelogRows& rows, u32 row) noexcept {
    if (row >= rows.count || !rows.rows[row].commit) return {};
    return {UiSettingsListStyle::row, UiIcon::code, UiIcon::count, {UiIcon::count, UiIcon::count}, false};
}

bool application_updates_initialize(const UpdateServiceConfig& config, Utf8View commit) noexcept {
    if (g_worker) return true;
    auto* w = static_cast<Worker*>(VirtualAlloc(nullptr, sizeof(Worker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w) return false;
    w->config = config;
    InitializeSRWLock(w->lock.native());
    w->view.own_version = config.own_version;
    if (commit.bytes >= kApplicationUpdatesCommitChars) copy_bytes(w->view.own_commit, commit.data, kApplicationUpdatesCommitChars);
    w->view.state = config.configured ? State::idle : State::disabled;
    const bool located = update_paths_default(&w->paths);
    w->view.channel = located ? update_channel_load(w->paths, config.default_channel) : config.default_channel;
    w->view.interval = located ? update_interval_load(w->paths, kUpdateDefaultCheckInterval) : kUpdateDefaultCheckInterval;
    if (config.configured && !located) { w->view.state = State::error; w->view.error = UpdateServiceError::storage; }
    g_view = w->view;
    g_worker = w;
    UpdateLog("updater").field("configured", config.configured).field("located", located).field("own", config.own_version)
        .field("channel", static_cast<u8>(w->view.channel))
        .field("interval", static_cast<u8>(w->view.interval)).write();
    if (!config.configured || !located) return true;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->wake && w->stop && w->notify && update_service_initialize(&w->service, config, w->paths) == UpdateServiceError::none &&
        update_https_initialize(&w->https)) {
        w->service.changelog = &w->download;
        w->service.observer = {w, offered};
        w->thread = CreateThread(nullptr, 256 * 1024, worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
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
void application_updates_ready() noexcept {
    Worker* w = g_worker;
    if (!w || !w->thread || g_ready_reported) return;
    g_ready_reported = true;
    AcquireSRWLockExclusive(w->lock.native());
    w->ready_requested = true;
    ReleaseSRWLockExclusive(w->lock.native());
    SetEvent(w->wake);
}
HANDLE application_updates_notification() noexcept { return g_worker ? g_worker->notify : nullptr; }
bool application_updates_poll() noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockShared(w->lock.native());
    const u64 revision = w->revision;
    const bool quit = w->quit;
    if (revision != g_seen_revision) {
        g_view = w->view;
        auto& rows = w->rows;
        // Row text is rebuilt only when the changelog or the versions it is
        // measured against change.
        if (!rows.built || rows.built_revision != g_view.changelog_revision || rows.built_own != g_view.own_version ||
            rows.built_offer != g_view.offer_version)
            application_updates_changelog_build(g_view, w->published, &rows);
    }
    ReleaseSRWLockShared(w->lock.native());
    if (revision != g_seen_revision) {
        g_seen_revision = revision;
        application_settings_refresh_pages();
        // The caption's update badge follows the view.
        application_request_frame();
    }
    if (quit && !g_quit_reported) { g_quit_reported = true; return true; }
    return false;
}
bool application_updates_project(SettingBinding binding, UiSettingsControl* control, UiSettingsControlPresentation* presentation,
    u16* meter) noexcept {
    if (binding == SettingBinding::update_changelog)
        return g_worker && application_updates_changelog_heading(g_worker->rows, control, nullptr);
    u32 slot = 6;
    switch (binding) {
    case SettingBinding::update_status: slot = 0; break;
    case SettingBinding::update_channel: slot = 1; break;
    case SettingBinding::update_rollback: slot = 2; break;
    case SettingBinding::update_notice: slot = 3; break;
    case SettingBinding::update_available: slot = 4; break;
    default: break;
    }
    return application_updates_project_view(g_view, binding, &g_text[slot], control, presentation, meter);
}
u32 application_updates_settings_list(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    return g_worker ? application_updates_changelog_project(g_worker->rows, controls, presentation) : 0;
}
UiSettingsListItem application_updates_list_item(u64 control_id, bool heading) noexcept {
    UiSettingsListItem item{};
    if (!g_worker) return item;
    if (heading) {
        UiSettingsControl control{};
        application_updates_changelog_heading(g_worker->rows, &control, &item);
    } else if (application_updates_changelog_row(control_id))
        item = application_updates_changelog_item(g_worker->rows, static_cast<u32>(control_id & 0xff));
    return item;
}
u16 application_updates_meter() noexcept {
    UiSettingsControl control{};
    UiSettingsControlPresentation presentation{};
    u16 meter = kUiSettingsNoMeter;
    application_updates_project_view(g_view, SettingBinding::update_available, &g_text[5], &control, &presentation, &meter);
    return meter;
}
ApplicationUpdatesSettingsBank* application_updates_settings_bank() noexcept {
    return g_worker ? &g_worker->bank : nullptr;
}
ApplicationUpdatesView application_updates_view() noexcept {
    return g_view;
}
bool application_updates_restart() noexcept {
    return application_update_badge_restarts(g_view.state) &&
           application_updates_activate(SettingBinding::update_status, 0);
}
bool application_updates_activate(SettingBinding binding, u32 choice) noexcept {
    Worker* w = g_worker;
    if (!w || !w->thread) return false;
    AcquireSRWLockExclusive(w->lock.native());
    const auto state = w->view.state;
    bool accepted = !busy_state(state);
    if (binding == SettingBinding::update_interval) {
        // A running check does not hold the interval; the worker applies it afterwards.
        accepted = choice < kUpdateCheckIntervalCount;
        if (accepted && choice != static_cast<u32>(w->view.interval)) {
            w->view.interval = static_cast<UpdateCheckInterval>(choice); w->interval_requested = true;
            ++w->revision;
        }
    } else if (accepted) {
        if (binding == SettingBinding::update_status) {
            if (state == State::ready) w->restart_requested = true; else w->check_requested = true;
        } else if (binding == SettingBinding::update_rollback && w->view.rollback_version) w->revert_requested = true;
        else if (binding == SettingBinding::update_channel && choice <= 1 && choice != static_cast<u32>(w->view.channel)) {
            // The other channel's offer and changelog are unknown until its check.
            w->view.channel = static_cast<UpdateChannel>(choice); w->channel_requested = true;
            clear_offer(w);
            ++w->revision;
        } else accepted = false;
    }
    ReleaseSRWLockExclusive(w->lock.native());
    if (accepted) { SetEvent(w->wake); SetEvent(w->notify); }
    return accepted;
}

} // namespace maicord::app_detail

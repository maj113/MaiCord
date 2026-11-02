#include "updates.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "../settings/settings.hpp"
#include "../shell/state.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/app/update/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/widgets/icon.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process_launch.hpp"
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
    bool check_requested, channel_requested, interval_requested, ready_requested, restart, quit;
    UpdateChangelogDownload download; // worker thread
    UpdateChangelog published;        // guarded by lock
    ApplicationUpdatesSettingsBank bank;  // UI thread
};
Worker* g_worker;
ApplicationUpdatesView g_view;
ApplicationUpdatesText g_text[4];
// The page's What's new rows; allocated while the Updates page is shown.
ApplicationUpdatesChangelogRows* g_rows;
u64 g_seen_revision;
bool g_quit_reported, g_ready_reported;
// The executable a Restart starts once the exit swapped it; empty otherwise.
wchar_t g_relaunch[kUpdatePathCapacity];
u32 g_relaunch_count;

bool busy_state(State state) noexcept {
    return state == State::checking || state == State::downloading || state == State::installing ||
           state == State::restarting;
}
u64 now_unix() noexcept { return clock_unix_milliseconds() / 1000; }
u32 random32() noexcept { u32 value = 0; secure_random_u32(&value); return value; }
void publish(Worker* w, void (*change)(Worker*, void*), void* context) noexcept {
    AcquireSRWLockExclusive(w->lock.native());
    const ApplicationUpdatesView before = w->view;
    const bool quit = w->quit;
    change(w, context);
    // A change that leaves the view and quit flag as they were (every published changelog bumps
    // changelog_revision) would only wake the UI to rebuild the same rows. The first
    // publication always goes out so the UI builds its rows once.
    const bool changed = w->revision == 0 || quit != w->quit || !bytes_equal(&before, &w->view, sizeof(before));
    if (changed) ++w->revision;
    ReleaseSRWLockExclusive(w->lock.native());
    if (changed) SetEvent(w->notify);
}
void set_state(Worker* w, State state, UpdateServiceError error = UpdateServiceError::none) noexcept {
    struct C { State s; UpdateServiceError e; } c{state, error};
    publish(w, [](Worker* worker, void* p) noexcept {
        const auto& c = *static_cast<C*>(p);
        worker->view.state = c.s; worker->view.error = c.e;
        if (c.s != State::downloading && c.s != State::installing) worker->view.received = worker->view.total = 0;
    }, &c);
}
// Under the lock: forget the offer.
void clear_offer(Worker* w) noexcept {
    auto& v = w->view;
    v.offer_version = v.offer_bytes = v.offer_date = 0;
    clear_bytes(v.offer_commit, sizeof(v.offer_commit));
}
// Under the lock: the changelog the page shows; null forgets it.
void set_changelog(Worker* w, const UpdateChangelog* log) noexcept {
    auto& v = w->view;
    auto& out = w->published;
    if (!log) {
        if (out.version) ++v.changelog_revision;
        out.version = out.count = out.text_bytes = 0;
        return;
    }
    if (out.version == log->version && out.channel == log->channel) return;
    out.version = log->version; out.base = log->base; out.channel = log->channel;
    out.count = log->count; out.text_bytes = log->text_bytes;
    copy_bytes(out.entries, log->entries, log->count * static_cast<u32>(sizeof(log->entries[0])));
    copy_bytes(out.text, log->text, log->text_bytes);
    ++v.changelog_revision;
}
// Worker: the cached changelog of the channel the page shows, before any check.
void publish_cached(Worker* w, UpdateChannel channel) noexcept {
    const UpdateChangelog* log = update_service_cached_changelog(&w->service, channel);
    if (!log) return;
    publish(w, [](Worker* worker, void* p) noexcept {
        if (worker->view.channel == static_cast<const UpdateChangelog*>(p)->channel)
            set_changelog(worker, static_cast<const UpdateChangelog*>(p));
    }, const_cast<UpdateChangelog*>(log));
}
// Worker: the channel's verified manifest, with the changelog the service
// holds for it, before admission and download.
void verified(void* context, const UpdateManifest& manifest, const UpdateChangelog* changelog, bool newer) noexcept {
    struct C { const UpdateManifest* m; const UpdateChangelog* log; bool newer; } c{&manifest, changelog, newer};
    publish(static_cast<Worker*>(context), [](Worker* worker, void* p) noexcept {
        const auto& c = *static_cast<C*>(p);
        auto& v = worker->view;
        if (c.log) set_changelog(worker, c.log);
        if (!c.newer) return;
        clear_offer(worker);
        v.offer_version = c.m->version; v.offer_bytes = c.m->artifact_bytes;
        if (!c.log) return;
        for (u32 i = 0; i < c.log->count; ++i) {
            const auto& entry = c.log->entries[i];
            if (entry.version != c.m->version) continue;
            v.offer_date = entry.date_unix_seconds;
            if (entry.has_commit) copy_bytes(v.offer_commit, entry.commit, kApplicationUpdatesCommitChars);
            break;
        }
    }, &c);
}
// Wraps the HTTPS transport to report download progress at a bounded rate.
struct Relay { Worker* worker; UpdateBodySink sink; void* context; };
bool relay_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* relay = static_cast<Relay*>(context);
    if (!relay->sink(relay->context, bytes)) return false;
    Worker* w = relay->worker;
    const u64 now = clock_monotonic_milliseconds();
    const LONG64 total = read_acquire(&w->service.progress.total);
    const LONG64 received = read_acquire(&w->service.progress.received);
    // The last bytes switch to "installing" at once: verification and staging
    // follow, and a stale percentage would read as a stall.
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
    // A stage verified by an earlier check stays ready, whatever this check
    // found, until the user installs it with Restart to update.
    const u64 staged = w->service.staged.file ? w->service.staged.version : 0;
    if (result.outcome == UpdateCheckOutcome::failed) ++w->failures;
    else w->failures = 0;
    if (result.outcome == UpdateCheckOutcome::failed && !staged) {
        set_state(w, result.error == UpdateServiceError::rate_limited ? State::rate_limited : State::error, result.error);
    } else {
        struct C { const UpdateCheckResult* r; u64 staged, now; } c{&result, staged, now_unix()};
        publish(w, [](Worker* worker, void* p) noexcept {
            const auto& c = *static_cast<C*>(p);
            auto& v = worker->view;
            v.error = UpdateServiceError::none; v.received = v.total = 0;
            if (c.r->outcome != UpdateCheckOutcome::failed) v.checked_unix = c.now;
            const bool ready = c.r->outcome == UpdateCheckOutcome::ready || c.staged;
            v.state = ready ? State::ready : State::up_to_date;
            if (c.r->outcome == UpdateCheckOutcome::ready) v.ready_version = c.r->version;
            else if (c.staged) v.ready_version = c.staged;
            else clear_offer(worker);
        }, &c);
    }
    return result.retry_at_unix;
}
UpdateCheckInterval current_interval(Worker* w) noexcept {
    return *static_cast<const volatile UpdateCheckInterval*>(&w->view.interval); // one byte: the lock added nothing
}
DWORD WINAPI worker_main(void* parameter) noexcept {
    auto* w = static_cast<Worker*>(parameter);
    auto& schedule = w->schedule;
    schedule.next_unix = now_unix() + kFallbackCheckDelaySeconds + random32() % kFallbackCheckJitterSeconds;
    publish_cached(w, w->view.channel);
    for (;;) {
        const u64 now = now_unix();
        const DWORD timeout = update_schedule_due(schedule, now) ? 0
            : static_cast<DWORD>(minimum<u64>(schedule.next_unix - now, 3600) * 1000);
        HANDLE handles[]{w->stop, w->wake};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
        AcquireSRWLockExclusive(w->lock.native());
        const bool check = w->check_requested, quit = w->quit;
        const bool channel_changed = w->channel_requested, interval_changed = w->interval_requested, ready = w->ready_requested;
        const UpdateChannel channel = w->view.channel;
        const UpdateCheckInterval interval = w->view.interval;
        w->check_requested = w->channel_requested = w->interval_requested = w->ready_requested = false;
        ReleaseSRWLockExclusive(w->lock.native());
        if (channel_changed) {
            update_channel_save(w->paths, channel);
            publish_cached(w, channel);
        }
        if (ready && !schedule.checked_unix)
            schedule.next_unix = minimum(schedule.next_unix, update_first_check(now_unix(), random32()));
        if (interval_changed) {
            update_interval_save(w->paths, interval);
            update_schedule_retarget(&schedule, update_check_interval_seconds(interval), random32());
        }
        // Restart to update: the window closes and the exit swaps the stage in.
        if (quit) break;
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
        t.number(tenths / 10); t.add("."); t.number(tenths % 10); t.add(" MB");
    } else {
        t.number((bytes + 1023) / 1024); t.add(" KB");
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

u32 application_updates_offer_label(const ApplicationUpdatesView& v, Slice<char> output) noexcept {
    const u64 version = offered_version(v);
    // The commit names the offer only when the changelog described this version.
    return version ? update_build_version_text(version, version == v.offer_version ? commit_of(v.offer_commit) : Utf8View{}, output) : 0;
}

namespace {
constexpr u8 icon_of(UiIcon icon) noexcept { return static_cast<u8>(static_cast<u8>(icon) + 1); }
// "Today at 2:02 PM" reads "today at 2:02 PM" inside a sentence.
void add_when(TextBank& t, u64 unix_seconds, u64 now) noexcept {
    char when[64];
    const auto formatted = format_timestamp_calendar(unix_seconds, now, {when, sizeof(when)});
    if (!formatted.ok || !formatted.bytes) return add_date(t, unix_seconds, true);
    if (formatted.bytes > 9 && (bytes_equal(when, "Today at ", 9) || bytes_equal(when, "Yesterday at ", 13)))
        when[0] = static_cast<char>(when[0] + 32);
    t.add({when, formatted.bytes});
}
// "MaiCord 0.5.4 · Stable channel"
void add_identity(TextBank& t, const ApplicationUpdatesView& v) noexcept {
    t.add("MaiCord "); add_build(t, v.own_version, own_commit(v));
    t.add(v.channel == UpdateChannel::preview ? utf8_literal(" · Preview channel") : utf8_literal(" · Stable channel"));
}
void add_checked(TextBank& t, const ApplicationUpdatesView& v, u64 now, bool past) noexcept {
    t.add(" · ");
    if (!v.checked_unix) { t.add("Not checked yet"); return; }
    t.add(past ? utf8_literal("Checked ") : utf8_literal("Last checked "));
    add_when(t, v.checked_unix, now);
}
void set_chip_tone(UiSettingsControlPresentation* presentation, u32 chip, UiSettingsChipTone tone) noexcept {
    presentation->chip_tones = static_cast<u16>(presentation->chip_tones | static_cast<u32>(tone) << (chip * 2));
}
// The status card: one state, one action.
void project_status(const ApplicationUpdatesView& v, u64 now, ApplicationUpdatesText* text, UiSettingsControl* control,
    UiSettingsControlPresentation* presentation, u16* meter) noexcept {
    TextBank title{text->label, sizeof(text->label), 0};
    TextBank meta{text->description, sizeof(text->description), 0};
    TextBank caption{text->detail, sizeof(text->detail), 0};
    const u64 offer = offered_version(v);
    control->kind = UiSettingsControlKind::status;
    control->enabled = true; control->checked = false;
    reset_value(&control->choices); reset_value(&control->action_label);
    presentation->variant = UiSettingsVariant::standard; presentation->more = 0; presentation->chip_tones = 0;
    UiIcon icon = UiIcon::download;
    switch (v.state) {
    case State::disabled:
        icon = UiIcon::info;
        title.add("Updates aren't configured for this build");
        meta.add("MaiCord "); add_build(meta, v.own_version, own_commit(v)); meta.add(". Download new versions manually.");
        control->enabled = false;
        break;
    case State::idle:
    case State::checking:
    case State::up_to_date: {
        const bool checking = v.state == State::checking, current = v.state == State::up_to_date;
        if (current) { icon = UiIcon::circle_check; presentation->variant = UiSettingsVariant::positive; }
        title.add(checking ? utf8_literal("Checking for updates…") : current ? utf8_literal("You're up to date")
                                                                            : utf8_literal("Check for updates"));
        add_identity(meta, v); add_checked(meta, v, now, current);
        presentation->more |= ui_settings_more_bold_lead;
        control->action_label = checking ? utf8_literal("Checking…") : utf8_literal("Check now");
        control->enabled = !checking; control->checked = checking;
        if (checking && meter) *meter = kUiSettingsIndeterminateMeter;
        break;
    }
    case State::downloading: {
        presentation->variant = UiSettingsVariant::primary;
        title.add("Downloading MaiCord "); add_build(title, offer);
        TextBank action{text->chip_text, sizeof(text->chip_text), 0};
        action.add("Downloading");
        if (v.total) {
            action.add(" "); action.number(minimum<u64>(100, v.received * 100 / v.total)); action.add("%");
            add_size(meta, v.received); meta.add(" of "); add_size(meta, v.total);
            if (meter) *meter = static_cast<u16>(minimum<u64>(1000, v.received * 1000 / v.total));
        } else {
            meta.add("Starting…");
        }
        control->action_label = action.view(); control->enabled = false;
        presentation->more = ui_settings_more_progress_button;
        break;
    }
    case State::installing:
        presentation->variant = UiSettingsVariant::primary;
        title.add("Verifying the update…");
        meta.add("MaiCord "); add_build(meta, offer);
        control->action_label = utf8_literal("Verifying…"); control->enabled = false; control->checked = true;
        if (meter) *meter = 1000;
        break;
    case State::ready:
    case State::restarting: {
        const bool restarting = v.state == State::restarting;
        const u64 version = v.ready_version > v.own_version ? v.ready_version : offer;
        const bool built = update_version_channel(version) == UpdateChannel::preview;
        presentation->variant = UiSettingsVariant::primary;
        if (restarting) title.add("Restarting…");
        else if (built) { title.add("Preview build "); add_build(title, version); title.add(" is ready"); }
        else { title.add("MaiCord "); add_build(title, version); title.add(" is ready to install"); }
        // The date, commit and size belong to the offer when it is this version.
        const bool described = version == v.offer_version;
        meta.add("Installed: "); add_build(meta, v.own_version, own_commit(v)); meta.add(" · Downloaded and verified");
        presentation->more |= ui_settings_more_bold_lead;
        if (described && v.offer_date) {
            meta.add(built ? utf8_literal(" · Built ") : utf8_literal(" · Released "));
            add_date(meta, v.offer_date, true);
        }
        TextBank chips{text->chip_text, sizeof(text->chip_text), 0};
        u32 count = 0;
        text->chips[count++] = chips.add(built ? utf8_literal("Preview build") : utf8_literal("Stable release"));
        if (described && built && v.offer_commit[0]) text->chips[count++] = chips.add({v.offer_commit, kApplicationUpdatesCommitChars});
        if (described && v.offer_bytes) {
            const u32 start = chips.used;
            add_size(chips, v.offer_bytes);
            text->chips[count++] = chips.since(start);
            add_size(caption, v.offer_bytes); caption.add(" · takes a few seconds");
        } else caption.add("Takes a few seconds");
        set_chip_tone(presentation, count, ui_settings_chip_positive);
        text->chips[count++] = chips.add("Signature verified");
        control->choices = {text->chips, count};
        control->action_label = restarting ? utf8_literal("Restarting…") : utf8_literal("Restart to update");
        control->enabled = !restarting; control->checked = restarting;
        break;
    }
    case State::error:
    case State::rate_limited:
        icon = UiIcon::warning;
        presentation->variant = UiSettingsVariant::critical;
        title.add(v.state == State::rate_limited ? utf8_literal("Couldn't check for updates") : utf8_literal("Couldn't update"));
        meta.add(update_service_error_text(v.error));
        if (v.checked_unix) { meta.add(" · Last checked "); add_when(meta, v.checked_unix, now); }
        control->action_label = utf8_literal("Try again");
        break;
    }
    control->label = title.view(); control->description = meta.view(); control->detail = caption.view();
    presentation->icon_plus_one = icon_of(icon);
}
// The Update settings rows.
void project_setting(const ApplicationUpdatesView& v, SettingBinding binding, ApplicationUpdatesText* text,
    UiSettingsControl* control) noexcept {
    if (binding == SettingBinding::update_interval) {
        control->selected_choice = static_cast<u32>(v.interval);
        control->choices = {interval_names, kUpdateCheckIntervalCount};
        control->enabled = true;
        return;
    }
    TextBank description{text->description, sizeof(text->description), 0};
    control->selected_choice = static_cast<u32>(v.channel);
    control->choices = {channel_names, 2};
    control->enabled = !busy_state(v.state);
    if (v.channel == UpdateChannel::preview)
        description.add(utf8_literal("Every master build that passes CI, often several a day. New features first; expect rough edges."));
    else {
        description.add("Tested releases. Recommended for most people.");
        if (update_version_channel(v.own_version) == UpdateChannel::preview)
            description.add(" This preview build stays installed until a newer stable release comes out.");
    }
    control->description = description.view();
}
Utf8View first_line(Utf8View text) noexcept {
    u32 bytes = 0;
    while (bytes < text.bytes && text.data[bytes] != '\n') ++bytes;
    return {text.data, bytes};
}
bool entry_bit(const u64 (&bits)[2], u32 entry) noexcept { return entry < 128 && (bits[entry >> 6] >> (entry & 63) & 1) != 0; }
void flip_entry_bit(u64 (&bits)[2], u32 entry) noexcept { if (entry < 128) bits[entry >> 6] ^= u64{1} << (entry & 63); }
} // namespace

bool application_updates_project_view(const ApplicationUpdatesView& v, u64 now, SettingBinding binding, ApplicationUpdatesText* text,
    UiSettingsControl* control, UiSettingsControlPresentation* presentation, u16* meter) noexcept {
    if (!text || !control || !presentation) return false;
    if (meter) *meter = kUiSettingsNoMeter;
    const bool disabled = v.state == State::disabled;
    switch (binding) {
    case SettingBinding::update_status:
        project_status(v, now, text, control, presentation, meter);
        return true;
    case SettingBinding::update_channel:
    case SettingBinding::update_interval:
        project_setting(v, binding, text, control);
        return !disabled;
    case SettingBinding::update_notice:
        return !disabled;
    default:
        return false;
    }
}

void application_updates_changelog_build(const ApplicationUpdatesView& v, const UpdateChangelog& log,
    ApplicationUpdatesChangelogRows* out) noexcept {
    if (!out) return;
    out->count = 0; reset_value(&out->note); out->built = true;
    const u64 own = v.own_version, offer = offered_version(v);
    out->built_revision = v.changelog_revision; out->built_own = own; out->built_offer = offer; out->built_stamp = out->stamp;
    out->bank = {out->text, sizeof(out->text), 0};
    // The choices belong to one changelog version: a new one starts over.
    if (out->state_version != log.version) {
        out->state_version = log.version;
        out->flipped[0] = out->flipped[1] = out->unclipped[0] = out->unclipped[1] = 0;
        out->older_open = out->more_open = false;
    }
    // An offer is described only by the changelog of its own version.
    if (!log.version || !log.count || (offer && log.version != offer)) return;
    TextBank& bank = out->bank;
    const bool preview = log.channel == UpdateChannel::preview;
    // A build newer than the published notes (a local or unpublished build)
    // shows the feed's newest entry only.
    const bool ahead = own > log.version;
    const u32 limit = ahead ? 1 : log.count;
    const u32 room = kApplicationUpdatesChangelogRows - 2;
    const auto add = [&](UiSettingsControlKind kind, u32 id, Utf8View label, Utf8View description, Utf8View detail,
                         UiSettingsVariant tone, bool checked, u8 more) noexcept {
        out->rows[out->count++] = {label, description, detail, kind, tone, more, static_cast<u8>(id), checked};
    };
    {
        const u32 start = bank.used;
        if (ahead) bank.add("newer than the published notes");
        else if (own < log.version) {
            if (preview) {
                u32 newer = 0;
                for (u32 i = 0; i < log.count; ++i) newer += log.entries[i].version > own;
                bank.number(newer); bank.add(newer == 1 ? utf8_literal(" change since ") : utf8_literal(" changes since "));
            } else bank.add("since ");
            add_build(bank, own);
        } else bank.add(preview ? utf8_literal("in the build you're running") : utf8_literal("in the version you're running"));
        out->note = bank.since(start);
    }
    u32 hidden = 0, total = 0;
    if (!preview) {
        // One disclosure row per release, the newest open; the rest fold behind "N older releases".
        u32 k = 0;
        for (; k < limit; ++k) {
            if (k == kApplicationUpdatesVisibleReleases) {
                const u32 start = bank.used;
                bank.number(limit - k); bank.add(limit - k == 1 ? utf8_literal(" older release") : utf8_literal(" older releases"));
                add(UiSettingsControlKind::disclosure, kApplicationUpdatesRowOlder, bank.since(start), {}, {},
                    UiSettingsVariant::standard, out->older_open, 0);
                if (!out->older_open) break;
            }
            const auto& entry = log.entries[k];
            const bool open = ((k == 0 && !ahead) != entry_bit(out->flipped, k));
            if (out->count + (open ? 2u : 1u) > room) { hidden = limit - k; break; }
            const u32 start = bank.used;
            bank.add("MaiCord "); add_build(bank, entry.version);
            const Utf8View label = bank.since(start);
            Utf8View date{};
            if (entry.date_unix_seconds) { const u32 d = bank.used; add_date(bank, entry.date_unix_seconds, true); date = bank.since(d); }
            const bool fresh = entry.version > own, installed = entry.version == own;
            add(UiSettingsControlKind::disclosure, k, label, date,
                fresh ? utf8_literal("New") : installed ? utf8_literal("Installed") : Utf8View{},
                fresh ? UiSettingsVariant::primary : UiSettingsVariant::positive, open, 0);
            if (open)
                add(UiSettingsControlKind::markdown, kApplicationUpdatesRowNotes + k, {}, bank.add(update_changelog_text(log, entry)),
                    utf8_literal("Show all notes"), UiSettingsVariant::standard, entry_bit(out->unclipped, k), 0);
        }
    } else {
        // One plain row per commit with its build and date; a divider marks the installed build.
        const u32 visible = out->more_open ? room : kApplicationUpdatesVisibleCommits;
        bool divider = ahead;
        u32 shown = 0;
        for (u32 k = 0; k < limit; ++k) {
            const auto& entry = log.entries[k];
            const Utf8View subject = first_line(update_changelog_text(log, entry));
            if (!subject.bytes) continue;
            if (!divider && entry.version <= own) {
                divider = true;
                if (shown < visible) {
                    const u32 start = bank.used;
                    add_build(bank, own);
                    add(UiSettingsControlKind::disclosure, kApplicationUpdatesRowInstalled, utf8_literal("Installed"), {},
                        bank.since(start), UiSettingsVariant::positive, false, ui_settings_more_plain);
                    ++shown;
                }
            }
            ++total;
            if (shown >= visible) { ++hidden; continue; }
            const Utf8View copied = bank.add(subject);
            const u32 build = bank.used;
            bank.add("dev."); bank.number(entry.version % 1000);
            const Utf8View chip = bank.since(build);
            const u32 d = bank.used;
            add_date(bank, entry.date_unix_seconds, false);
            add(UiSettingsControlKind::disclosure, k, copied, bank.since(d), chip,
                entry.version == offer ? UiSettingsVariant::primary : UiSettingsVariant::standard, false, ui_settings_more_plain);
            ++shown;
        }
        // The installed build below every listed change.
        if (!divider && !hidden && own >= log.base && shown < visible) {
            const u32 start = bank.used;
            add_build(bank, own);
            add(UiSettingsControlKind::disclosure, kApplicationUpdatesRowInstalled, utf8_literal("Installed"), {}, bank.since(start),
                UiSettingsVariant::positive, false, ui_settings_more_plain);
        }
        if (!ahead && !out->more_open && hidden) {
            const u32 start = bank.used;
            bank.add("Show "); bank.number(hidden); bank.add(" more");
            add(UiSettingsControlKind::disclosure, kApplicationUpdatesRowMore, bank.since(start), {}, {}, UiSettingsVariant::standard,
                false, 0);
            hidden = 0;
        } else if (!ahead && out->more_open && total > kApplicationUpdatesVisibleCommits)
            add(UiSettingsControlKind::disclosure, kApplicationUpdatesRowMore, utf8_literal("Show fewer"), {}, {},
                UiSettingsVariant::standard, true, 0);
    }
    // A truncated list, and changelogs that start after this build, say so.
    const u32 start = bank.used;
    if (hidden) {
        bank.number(hidden);
        bank.add(preview ? utf8_literal(" earlier changes aren't shown.") : utf8_literal(" earlier releases aren't shown."));
    } else if (!ahead && own < log.base) {
        bank.add("Changes up to "); add_build(bank, log.base); bank.add(" aren't listed here.");
    }
    if (bank.used != start)
        add(UiSettingsControlKind::footnote, kApplicationUpdatesRowNote, bank.since(start), {}, {}, UiSettingsVariant::standard, false, 0);
}

bool application_updates_changelog_heading(const ApplicationUpdatesChangelogRows& rows, UiSettingsControl* control) noexcept {
    if (!rows.built || !rows.count || !control) return false;
    control->kind = UiSettingsControlKind::category;
    control->detail = rows.note;
    return true;
}

u32 application_updates_changelog_project(const ApplicationUpdatesChangelogRows& rows, Slice<UiSettingsControl> controls,
    Slice<UiSettingsControlPresentation> presentation) noexcept {
    if (!rows.built) return 0;
    u32 count = 0;
    for (u32 i = 0; i < rows.count && count < controls.count && count < presentation.count; ++i) {
        const auto& row = rows.rows[i];
        auto& control = controls[count];
        control = {};
        control.id = settings_list_control_id(SettingBinding::update_changelog, row.id);
        control.kind = row.kind; control.enabled = true; control.checked = row.checked;
        if (row.kind != UiSettingsControlKind::markdown) control.label = row.label;
        control.description = row.description; control.detail = row.detail;
        presentation[count] = {};
        presentation[count].variant = row.tone; presentation[count].more = row.more;
        presentation[count].default_choice = kUiSettingsNoDefault;
        ++count;
    }
    return count;
}

bool application_updates_changelog_toggle(ApplicationUpdatesChangelogRows* rows, u32 id) noexcept {
    if (!rows) return false;
    if (id < kApplicationUpdatesRowNotes) flip_entry_bit(rows->flipped, id);
    else if (id < kApplicationUpdatesRowNotes + kUpdateChangelogMaximumEntries) flip_entry_bit(rows->unclipped, id - kApplicationUpdatesRowNotes);
    else if (id == kApplicationUpdatesRowOlder) rows->older_open = !rows->older_open;
    else if (id == kApplicationUpdatesRowMore) rows->more_open = !rows->more_open;
    else return false;
    ++rows->stamp;
    return true;
}

u32 application_updates_release_notes_url(const ApplicationUpdatesView& v, Utf8View repository, Slice<char> output) noexcept {
    if (!repository.bytes) return 0;
    UpdateTextWriter url{output, 0, true};
    url.add(utf8_literal("https://github.com/")); url.add(repository);
    // A stable release has its own page; master builds share the releases page.
    const u64 offer = offered_version(v), version = offer ? offer : v.own_version;
    if (update_version_channel(version) == UpdateChannel::stable) {
        char tag[24];
        url.add(utf8_literal("/releases/tag/v")); url.add({tag, update_version_text(version, {tag, sizeof(tag)})});
    } else url.add(utf8_literal("/releases"));
    return url.ok ? url.used : 0;
}

bool application_updates_initialize(const UpdateServiceConfig& config, Utf8View commit) noexcept {
    if (g_worker) return true;
    auto* w = static_cast<Worker*>(VirtualAlloc(nullptr, sizeof(Worker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w) return false;
    w->config = config;
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
    if (located) update_paths_cleanup(w->paths);
    if (!config.configured || !located) return true;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->wake && w->stop && w->notify && update_service_initialize(&w->service, config, w->paths) == UpdateServiceError::none &&
        update_https_initialize(&w->https)) {
        w->service.changelog = &w->download;
        w->service.observer = {w, verified};
        w->thread = CreateThread(nullptr, 256 * 1024, worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    if (!w->thread) { w->view.state = State::error; w->view.error = UpdateServiceError::storage; g_view = w->view; }
    return w->thread != nullptr;
}
void application_updates_release(bool normal_exit) noexcept {
    Worker* w = g_worker;
    if (!w) return;
    g_worker = nullptr;
    application_updates_visible(false);
    const auto plan = application_updates_exit(normal_exit, w->restart);
    // Restart to update starts the executable path at the very end of the exit.
    if (plan.relaunch) {
        g_relaunch_count = w->paths.executable_count;
        copy_bytes(g_relaunch, w->paths.executable, (g_relaunch_count + 1) * sizeof(wchar_t));
    }
    if (w->thread) {
        SetEvent(w->stop);
        update_https_cancel(&w->https);
        // Staging in flight finishes first: never swap from or free storage
        // the worker may still touch.
        if (WaitForSingleObject(w->thread, 3000) != WAIT_OBJECT_0) return;
        CloseHandle(w->thread);
    }
    // Restart to update: the executable this process verified replaces the
    // running one, so the relaunch starts the new version. Any other exit
    // keeps the running version; the service keeps the package.
    if (plan.install) update_service_swap(&w->service);
    update_service_release(&w->service);
    update_https_release(&w->https);
    const HANDLE events[]{w->wake, w->stop, w->notify};
    for (HANDLE h : events) if (h) CloseHandle(h);
    VirtualFree(w, 0, MEM_RELEASE);
}
void application_updates_relaunch() noexcept {
    if (!g_relaunch_count) return;
    wchar_t command[kUpdatePathCapacity + 2];
    const u32 count = g_relaunch_count;
    command[0] = L'"';
    copy_bytes(command + 1, g_relaunch, count * sizeof(wchar_t));
    command[count + 1] = L'"'; command[count + 2] = 0;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(g_relaunch, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
    const DWORD native = started ? 0 : GetLastError();
    if (started) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    UpdateLog("relaunch").field("started", started).field("native", native).write(started ? LogLevel::info : LogLevel::warning);
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
    if (revision != g_seen_revision) g_view = w->view;
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
namespace {
// The page's changelog rows, built on first use and again whenever the
// changelog, the versions or the user's disclosure choices moved.
ApplicationUpdatesChangelogRows* page_rows() noexcept {
    Worker* w = g_worker;
    if (!w) return nullptr;
    if (!g_rows) g_rows = static_cast<ApplicationUpdatesChangelogRows*>(
        VirtualAlloc(nullptr, sizeof(ApplicationUpdatesChangelogRows), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    ApplicationUpdatesChangelogRows* rows = g_rows;
    if (rows && (!rows->built || rows->built_revision != g_view.changelog_revision || rows->built_own != g_view.own_version ||
                 rows->built_offer != offered_version(g_view) || rows->built_stamp != rows->stamp)) {
        AcquireSRWLockShared(w->lock.native());
        application_updates_changelog_build(g_view, w->published, rows);
        ReleaseSRWLockShared(w->lock.native());
    }
    return rows;
}
} // namespace
bool application_updates_project(SettingBinding binding, UiSettingsControl* control, UiSettingsControlPresentation* presentation,
    u16* meter) noexcept {
    if (binding == SettingBinding::update_changelog) {
        const auto* rows = page_rows();
        return rows && application_updates_changelog_heading(*rows, control);
    }
    if (binding == SettingBinding::update_release_notes) {
        const auto* rows = page_rows();
        char url[kApplicationUpdatesReleaseNotesBytes];
        return rows && rows->count && application_updates_release_notes({url, sizeof(url)});
    }
    u32 slot = 2;
    switch (binding) {
    case SettingBinding::update_status: slot = 0; break;
    case SettingBinding::update_channel: slot = 1; break;
    default: break;
    }
    return application_updates_project_view(g_view, now_unix(), binding, &g_text[slot], control, presentation, meter);
}
u32 application_updates_settings_list(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    const auto* rows = page_rows();
    return rows ? application_updates_changelog_project(*rows, controls, presentation) : 0;
}
u16 application_updates_meter() noexcept {
    UiSettingsControl control{};
    UiSettingsControlPresentation presentation{};
    u16 meter = kUiSettingsNoMeter;
    application_updates_project_view(g_view, now_unix(), SettingBinding::update_status, &g_text[3], &control, &presentation, &meter);
    return meter;
}
void application_updates_visible(bool visible) noexcept {
    if (visible || !g_rows) return;
    VirtualFree(g_rows, 0, MEM_RELEASE);
    g_rows = nullptr;
}
bool application_updates_activate_row(u64 control_id) noexcept {
    return application_updates_changelog_row(control_id) && g_rows &&
           application_updates_changelog_toggle(g_rows, static_cast<u32>(control_id & 0xff));
}
u32 application_updates_release_notes(Slice<char> output) noexcept {
    return g_worker ? application_updates_release_notes_url(g_view, {g_worker->config.repository, g_worker->config.repository_bytes}, output) : 0;
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
        if (binding == SettingBinding::update_status && state == State::ready) {
            // The window closes for real; its normal exit installs the stage.
            w->restart = w->quit = true;
            w->view.state = State::restarting;
            ++w->revision;
        } else if (binding == SettingBinding::update_status) w->check_requested = true;
        else if (binding == SettingBinding::update_channel && choice <= 1 && choice != static_cast<u32>(w->view.channel)) {
            // The other channel's offer and changelog are unknown until its check.
            w->view.channel = static_cast<UpdateChannel>(choice); w->channel_requested = true;
            clear_offer(w); set_changelog(w, nullptr);
            ++w->revision;
        } else accepted = false;
    }
    ReleaseSRWLockExclusive(w->lock.native());
    if (accepted) { SetEvent(w->wake); SetEvent(w->notify); }
    return accepted;
}

} // namespace maicord::app_detail

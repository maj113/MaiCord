#include "application_startup_shell.hpp"

#include "account_ui_runtime.hpp"
#include "application_account_logout.hpp"
#include "application_channel_authority.hpp"
#include "application_guild_folders.hpp"
#include "application_load_trace.hpp"
#include "application_state.hpp"

#include "maicord/app/startup_shell.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/core/win32_threadpool.hpp"

namespace maicord::app_detail {
namespace {

// Navigation bursts coalesce into one write; the first deadline stays fixed.
constexpr u32 kWriteDelayMs = 3'000;
constexpr u32 kRetryDelayMs = 30'000;
constexpr u32 kCollapsedCapacity = 64;

enum class Work : u8 { load, save };

// The pool work reads or writes `shell` with `key`; the UI thread touches
// neither while `busy` is set. The read is consumed by apply (or dropped).
// `shell` is allocated only for a read or a write.
struct StartupShellOwner {
    PTP_WORK work;
    volatile LONG busy;
    Work job;
    StartupShellOpenError loaded;
    bool loading, writing, saved;
    u64 profile_id, now_unix_ms;
    u64 write_at_ms;
    u64 shown_channel_id;
    u32 observed_selection;
    u32 collapsed_count;
    i64 load_begin, load_end;
    u64 collapsed[kCollapsedCapacity];
    StartupShellKey key;
    StartupShell* shell;
    wchar_t path[kAccountProfileMaximumPathCharacters];
};
StartupShellOwner* g_owner;
// READY ended a shell that replaced the restoring view.
bool g_release_pending;

void __stdcall run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    auto& owner = *static_cast<StartupShellOwner*>(context);
    if (owner.job == Work::load) {
        owner.load_begin = clock_now();
        owner.loaded = startup_shell_load_file(owner.path, owner.profile_id, owner.now_unix_ms, &owner.key,
                                               owner.shell);
        owner.load_end = clock_now();
    } else
        owner.saved = startup_shell_save_file(owner.path, &owner.key, *owner.shell);
    InterlockedExchange(&owner.busy, 0);
}

void submit(StartupShellOwner& owner, Work job) noexcept {
    owner.job = job;
    InterlockedExchange(&owner.busy, 1);
    SubmitThreadpoolWork(owner.work);
}

void wait(StartupShellOwner& owner) noexcept {
    WaitForThreadpoolWorkCallbacks(owner.work, FALSE);
}

// Fresh pages each time: a shell is only ever read whole or written whole.
bool allocate_shell(StartupShellOwner& owner) noexcept {
    owner.shell = static_cast<StartupShell*>(
        VirtualAlloc(nullptr, sizeof(StartupShell), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return owner.shell != nullptr;
}

void release_shell(StartupShellOwner& owner) noexcept {
    if (owner.shell)
        VirtualFree(owner.shell, 0, MEM_RELEASE);
    owner.shell = nullptr;
}

const char* open_error_name(StartupShellOpenError error) noexcept {
    switch (error) {
    case StartupShellOpenError::none: return "none";
    case StartupShellOpenError::format: return "absent_or_malformed";
    case StartupShellOpenError::version: return "version";
    case StartupShellOpenError::account: return "account";
    case StartupShellOpenError::key: return "key";
    case StartupShellOpenError::tamper: return "tamper";
    case StartupShellOpenError::malformed: return "malformed";
    case StartupShellOpenError::expired: return "expired";
    }
    return "unknown";
}

void release_owner() noexcept {
    if (!g_owner)
        return;
    wait(*g_owner);
    CloseThreadpoolWork(g_owner->work);
    startup_shell_key_release(&g_owner->key);
    release_shell(*g_owner);
    VirtualFree(g_owner, 0, MEM_RELEASE);
    g_owner = nullptr;
}

// What the shell records is READY's state, settled on its selection.
bool capturable() noexcept {
    const AccountUiRuntime* ui = g_app.account_ui;
    return ui && g_app.account.instance && g_app.account.status.running && !g_app.startup_shell_provisional &&
           ui->chat_state.self_user_id && !ui->chat_state.restore_channel_id &&
           g_app.account.status.profile_id == g_owner->profile_id;
}

bool capture(StartupShellOwner& owner) noexcept {
    if (!allocate_shell(owner))
        return false;
    AccountUiRuntime& ui = *g_app.account_ui;
    StartupShell& shell = *owner.shell;
    shell.profile_id = owner.profile_id;
    shell.saved_unix_ms = clock_unix_milliseconds();
    startup_shell_capture({&ui.chat_state, &ui.user_store, &ui.guild_member_store, ui.navigation_view.guild_folders},
                          ui.active_channel_id == ui.chat_state.selected_channel_id ? &g_app.message_view.store
                                                                                     : nullptr,
                          &shell);
    shell.folder_bytes = application_guild_folders_capture({shell.folders, kStartupShellFolderBytes},
                                                           {shell.expanded_folders, kStartupShellExpandedCapacity},
                                                           &shell.expanded_count);
    return true;
}

// Collects a finished write.
void collect(StartupShellOwner& owner, u64 now_ms) noexcept {
    if (!owner.writing || owner.busy)
        return;
    owner.writing = false;
    release_shell(owner);
    if (!owner.saved) {
        log_write(LogLevel::warning, "shell", "startup_shell_save_failed");
        if (!owner.write_at_ms)
            owner.write_at_ms = now_ms + kRetryDelayMs;
    }
}

} // namespace

void application_startup_shell_begin(const AccountPaths& paths, u64 profile_id) noexcept {
    release_owner();
    const u32 length = wide_length(paths.shell, kAccountProfileMaximumPathCharacters);
    if (!profile_id || !length)
        return;
    auto* owner = static_cast<StartupShellOwner*>(
        VirtualAlloc(nullptr, sizeof(StartupShellOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return;
    owner->work = CreateThreadpoolWork(run, owner, nullptr);
    if (!owner->work) {
        VirtualFree(owner, 0, MEM_RELEASE);
        return;
    }
    copy_bytes(owner->path, paths.shell, (length + 1) * static_cast<u32>(sizeof(wchar_t)));
    owner->profile_id = profile_id;
    owner->now_unix_ms = clock_unix_milliseconds();
    g_owner = owner;
    owner->loading = allocate_shell(*owner);
    if (owner->loading)
        submit(*owner, Work::load);
}

bool application_startup_shell_apply() noexcept {
    g_app.startup_shell_provisional = g_release_pending = false;
    StartupShellOwner* owner = g_owner;
    if (!owner || !g_app.account_ui || !owner->loading)
        return false;
    const i64 wait_begin = clock_now();
    wait(*owner);
    const i64 apply_begin = clock_now();
    owner->loading = false;
    AccountUiRuntime& ui = *g_app.account_ui;
    if (owner->loaded != StartupShellOpenError::none || g_app.account.status.profile_id != owner->profile_id) {
        log_write(LogLevel::info, "shell", open_error_name(owner->loaded));
        release_shell(*owner);
        return false;
    }
    const StartupShell& shell = *owner->shell;
    const bool placed = startup_shell_apply(shell, {&ui.chat_state, &ui.user_store, &ui.guild_member_store});
    if (!application_guild_folders_restore({shell.folders, shell.folder_bytes},
                                           {shell.expanded_folders, shell.expanded_count}))
        log_write(LogLevel::warning, "shell", "startup_shell_folders_rejected");
    owner->collapsed_count = 0;
    for (u32 i = 0; i < shell.channel_count && owner->collapsed_count < kCollapsedCapacity; ++i)
        if (shell.channels[i].type == 4 && (shell.channels[i].state & startup_shell_channel_collapsed))
            owner->collapsed[owner->collapsed_count++] = shell.channels[i].id;
    owner->shown_channel_id = ui.chat_state.selected_channel_id;
    owner->observed_selection = ui.chat_state.selection_revision;
    g_app.startup_shell_provisional = true;
    // Counts and timings only: the pool thread's read, unwrap, decrypt and
    // decode; how long attach waited for it; and the stores' fill.
    TraceRecord line("startup_shell_applied");
    line.field("load_us", clock_microseconds(g_app.clock, owner->load_begin, owner->load_end));
    line.field("wait_us", clock_microseconds(g_app.clock, wait_begin, apply_begin));
    line.field("apply_us", clock_microseconds(g_app.clock, apply_begin, clock_now()));
    line.field("guilds", shell.guild_count);
    line.field("channels", shell.channel_count);
    line.field("conversations", shell.conversation_count);
    line.field("users", shell.user_count);
    line.field("selection_placed", placed ? u64{1} : u64{0});
    line.field("folder_bytes", shell.folder_bytes);
    line.write("load_trace");
    release_shell(*owner);
    return true;
}

bool application_startup_shell_gateway_ready(u64 self_user_id) noexcept {
    if (!g_app.startup_shell_provisional || !g_app.account_ui ||
        g_app.account_ui->chat_state.self_user_id == self_user_id)
        return false;
    g_app.account_ui->chat_state.self_user_id = 0;
    return true;
}

void application_startup_shell_ready() noexcept {
    if (!g_app.startup_shell_provisional || !g_app.account_ui)
        return;
    g_app.startup_shell_provisional = false;
    g_release_pending = true;
    if (!g_owner)
        return;
    AccountUiRuntime& ui = *g_app.account_ui;
    ChatStateStore& state = ui.chat_state;
    for (u32 i = 0; i < g_owner->collapsed_count; ++i) {
        const ChannelRecord* category = chat_state_store_find_channel(state, g_owner->collapsed[i]);
        if (!category || category->type != 4 || category->collapsed)
            continue;
        ChannelRecord& record = state.channels[category - state.channels];
        record.collapsed = true;
        saturating_increment(record.revision);
        saturating_increment(state.channel_mutations);
    }
    // A selection READY moved has already reset the timeline. A shown channel
    // READY no longer lets this account read loses its page at once.
    const u64 channel = ui.active_channel_id;
    MessageHistoryCatchup& catchup = g_app.message_catchup;
    if (!channel || channel != g_owner->shown_channel_id)
        return;
    if (!application_channel_readable(g_app, channel)) {
        reset_value(&catchup);
        reset_value(&g_app.message_older);
        if (!message_timeline_reset(&g_app.message_view.timeline) ||
            !message_timeline_set_scroll(&g_app.message_view.timeline, 0, true))
            log_write(LogLevel::error, "shell", "startup_page_clear_failed");
        return;
    }
    // READY asks for a full resync of retained history and drops the old
    // catch-up. The page shown before READY came from this session's
    // handshake, as a held startup page would: when the restoration kept its
    // channel, fetch only what is newer.
    if (channel == state.selected_channel_id && !state.restore_channel_id &&
        ui.observed_selection_revision == state.selection_revision && g_app.message_view.store.count &&
        catchup.state == MessageHistoryCatchupState::idle) {
        g_app.message_pump.catchup_required = true;
        if (!message_history_catchup_begin(&catchup, &g_app.message_pump, channel, kMessageHistoryRequestLimit))
            log_write(LogLevel::info, "shell", "startup_page_resynced");
        TraceRecord kept("startup_page_kept");
        kept.field("newer_only", catchup.cursor_after ? u64{1} : u64{0});
        kept.write("load_trace");
    }
}

bool application_startup_shell_take_release() noexcept {
    const bool pending = g_release_pending;
    g_release_pending = false;
    return pending;
}

void application_startup_shell_pump(u64 now_ms) noexcept {
    StartupShellOwner* owner = g_owner;
    if (!owner || !g_app.account_ui || owner->busy)
        return;
    // A read nobody applied (a restarted session keeps its shown state).
    if (owner->loading) {
        owner->loading = false;
        release_shell(*owner);
    }
    collect(*owner, now_ms);
    const u32 selection = g_app.account_ui->chat_state.selection_revision;
    if (owner->observed_selection != selection) {
        owner->observed_selection = selection;
        if (!owner->write_at_ms && !g_app.startup_shell_provisional)
            owner->write_at_ms = now_ms + kWriteDelayMs;
    }
    if (!owner->write_at_ms || now_ms < owner->write_at_ms)
        return;
    if (!capturable()) {
        owner->write_at_ms = now_ms + kWriteDelayMs;
        return;
    }
    owner->write_at_ms = 0;
    if (!capture(*owner))
        return;
    owner->writing = true;
    submit(*owner, Work::save);
}

u64 application_startup_shell_next_wake() noexcept {
    const StartupShellOwner* owner = g_owner;
    if (!owner || !g_app.account_ui || owner->busy)
        return 0;
    if (owner->observed_selection != g_app.account_ui->chat_state.selection_revision)
        return 1;
    return owner->write_at_ms;
}

void application_startup_shell_stop() noexcept {
    StartupShellOwner* owner = g_owner;
    if (!owner)
        return;
    wait(*owner);
    release_shell(*owner);
    if (!application_account_logout_blocked() && capturable() && capture(*owner) &&
        !startup_shell_save_file(owner->path, &owner->key, *owner->shell))
        log_write(LogLevel::warning, "shell", "startup_shell_save_failed");
    release_owner();
}

} // namespace maicord::app_detail

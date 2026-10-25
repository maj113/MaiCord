#include "application_startup_history.hpp"

#include "application_channel_authority.hpp"
#include "application_load_trace.hpp"
#include "application_state.hpp"

#include "maicord/app/channel_memory.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

enum class StartupHistoryPhase : u8 {
    idle,
    requested,
    held
};

struct StartupHistory {
    RestClientResponse response;
    u64 channel_id;
    u64 request_id;
    u8* body;
    u32 body_bytes;
    StartupHistoryPhase phase;
};
StartupHistory g_startup;

// The account UI's first PRIMARY ID; attach advances its counter past it.
constexpr u64 kStartupHistoryRequestId = 1;

void release_body() noexcept {
    if (g_startup.body) {
        secure_clear_bytes(g_startup.body, g_startup.body_bytes);
        VirtualFree(g_startup.body, 0, MEM_RELEASE);
    }
    g_startup.body = nullptr;
    g_startup.body_bytes = 0;
}

void finish(const char* outcome, const RestClientResponse& response) noexcept {
    TraceRecord line("history_prefetch");
    line.field("request", g_startup.request_id);
    line.field("outcome", outcome);
    line.field("status", response.status_code);
    line.field("elapsed_us", response.elapsed_us);
    line.write("load_trace");
    release_body();
    reset_value(&g_startup);
}

// READY has placed (or abandoned) the restoration target and the channel
// session observed the result.
bool restoration_settled() noexcept {
    const AccountUiRuntime* ui = g_app.account_ui;
    return ui && !ui->chat_state.restore_channel_id &&
           ui->observed_selection_revision == ui->chat_state.selection_revision;
}

// The catch-up the selected channel would submit is exactly the query sent.
// READY with a pending restoration requests a resync, which only matters
// when messages are retained.
bool catchup_matches(u64 channel_id) noexcept {
    const AccountUiRuntime& ui = *g_app.account_ui;
    const MessageHistoryCatchup& catchup = g_app.message_catchup;
    return channel_id && ui.active_channel_id == channel_id &&
           ui.chat_state.selected_channel_id == channel_id && catchup.channel_id == channel_id &&
           catchup.state == MessageHistoryCatchupState::request_required && catchup.cursor_after == 0 &&
           catchup.request_limit == kMessageHistoryRequestLimit && !g_app.pending_message_jump_message_id &&
           (!g_app.message_pump.resync_required || g_app.message_view.store.count == 0) &&
           application_channel_readable(g_app, channel_id);
}

// A page that answered before READY can miss messages created before READY.
// A full page is followed by an after-newest request anyway; a short page
// must reach the channel's last message id.
bool page_current(u64 channel_id, Utf8View body) noexcept {
    AccountUiRuntime& ui = *g_app.account_ui;
    MessageHistoryDecodeScratch scratch{};
    if (!application_message_decode_scratch(ui, &scratch, body))
        return false;
    const MessageHistoryDecodeResult decoded = decode_message_history_page(
        body, scratch.page_structurals, scratch.message_structurals, scratch.maximum_depth, scratch.content,
        scratch.messages, scratch.mention_users, scratch.rich);
    if (!decoded.ok() || (decoded.page.count && decoded.page.channel_id != channel_id))
        return false;
    const ChannelRecord* channel = chat_state_store_find_channel(ui.chat_state, channel_id);
    return channel && (decoded.page.count >= kMessageHistoryRequestLimit ||
                       channel->last_message_id <= decoded.page.newest_id);
}

} // namespace

bool application_startup_history_submit(u64 channel_id) noexcept {
    AccountHost& host = g_app.account;
    release_body();
    reset_value(&g_startup);
    if (!channel_id || !host.instance || !host.api.history_submit)
        return false;
    const MessageHistoryQuery query{0, kMessageHistoryRequestLimit, MessageHistoryPosition::latest};
    if (!host.api.history_submit(host.instance, channel_id, &query, kStartupHistoryRequestId).ok())
        return false;
    g_startup.channel_id = channel_id;
    g_startup.request_id = kStartupHistoryRequestId;
    g_startup.phase = StartupHistoryPhase::requested;
    application_trace_load_stage("history_prefetch_submitted");
    return true;
}

void application_startup_history_begin(u64 profile_id) noexcept {
    wchar_t path[512];
    ChannelMemory memory{};
    if (!channel_memory_path(profile_id, path, 512) || !channel_memory_load_file(path, &memory))
        return;
    if (const ChannelMemoryEntry* latest = channel_memory_latest(memory))
        application_startup_history_submit(latest->channel_id);
}

void application_startup_history_attach() noexcept {
    if (g_startup.phase != StartupHistoryPhase::requested)
        return;
    if (!g_app.account_ui) {
        reset_value(&g_startup);
        return;
    }
    u64& counter = g_app.account_ui->message_history_request_id;
    if (counter < g_startup.request_id)
        counter = g_startup.request_id;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::startup_history;
    g_app.primary_request_id = g_startup.request_id;
}

bool application_startup_history_take(const RestClientResponse& response, Utf8View body) noexcept {
    if (g_startup.phase != StartupHistoryPhase::requested || response.request_id != g_startup.request_id ||
        !g_app.account_ui) {
        release_body();
        reset_value(&g_startup);
        return false;
    }
    if (!response.transport_ok() || response.status_code != 200 || !body.data || !body.bytes ||
        body.bytes != response.body_bytes) {
        finish("failed", response);
        return false;
    }
    if (!restoration_settled()) {
        auto* copy =
            static_cast<u8*>(VirtualAlloc(nullptr, body.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!copy) {
            finish("dropped_memory", response);
            return false;
        }
        copy_bytes(copy, body.data, body.bytes);
        g_startup.body = copy;
        g_startup.body_bytes = body.bytes;
        g_startup.response = response;
        g_startup.phase = StartupHistoryPhase::held;
        TraceRecord line("history_prefetch");
        line.field("request", g_startup.request_id);
        line.field("outcome", "held");
        line.field("elapsed_us", response.elapsed_us);
        line.write("load_trace");
        return false;
    }
    if (!catchup_matches(g_startup.channel_id)) {
        finish("selection_changed", response);
        return false;
    }
    if (!page_current(g_startup.channel_id, body)) {
        finish("stale", response);
        return false;
    }
    MessageHistoryQuery query{};
    if (!message_history_catchup_take_query(&g_app.message_catchup, &query)) {
        finish("selection_changed", response);
        return false;
    }
    g_app.message_pump.resync_required = false;
    finish("adopted", response);
    return true;
}

bool application_startup_history_replay_due() noexcept {
    return g_startup.phase == StartupHistoryPhase::held && restoration_settled();
}

RestClientTakeResult application_startup_history_replay(RestClientResponse* response,
                                                        Slice<u8> output) noexcept {
    if (!response || !application_startup_history_replay_due())
        return {0, RestClientTakeError::empty};
    if (g_startup.body_bytes > output.count) {
        finish("dropped_size", g_startup.response);
        return {0, RestClientTakeError::empty};
    }
    const u32 bytes = g_startup.body_bytes;
    copy_bytes(output.data, g_startup.body, bytes);
    *response = g_startup.response;
    release_body();
    g_startup.phase = StartupHistoryPhase::requested;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::startup_history;
    g_app.primary_request_id = g_startup.request_id;
    return {bytes, RestClientTakeError::none};
}

u64 application_startup_history_next_wake() noexcept {
    return application_startup_history_replay_due() ? 1 : 0;
}

void application_startup_history_reset() noexcept {
    if (g_app.message_history_request_owner == MessageHistoryRequestOwner::startup_history) {
        g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
        g_app.primary_request_id = 0;
    }
    release_body();
    reset_value(&g_startup);
}

} // namespace maicord::app_detail

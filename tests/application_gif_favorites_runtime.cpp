// Shipping broker with synthetic account REST receipts only. No account,
// credentials, network worker, catalog fetch, or cloud mutation is used.
#include "../src/app/application_gif_favorites.cpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/trace_record.hpp"
#include "test_output.hpp"

namespace maicord::gif_favorites_runtime_test {
using namespace app_detail;
u32 failures, submits, cancels, callbacks;
u64 submitted_id, cancelled_id;
AccountGifFavoritesOperation submitted_operation;
RestClientSubmitError admission;
u8 captured_patch[kGifFavoritesMaximumPatchBytes];
u32 captured_bytes;
FavoriteBank* watch_bank;
bool callback_before_free;
const void* transient_base;
usize transient_bytes;
constexpr auto saved = utf8_literal("https://example.test/saved.gif");
constexpr auto another = utf8_literal("https://example.test/new.gif");
constexpr auto third = utf8_literal("https://example.test/third.gif");
constexpr auto source = utf8_literal("https://example.test/preview.gif");
void check(bool pass, const char* message) noexcept {
    test_check(&failures, pass, message);
}
bool is_free(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(pointer, &memory, sizeof(memory)) && memory.State == MEM_FREE;
}
usize committed(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return pointer && VirtualQuery(pointer, &memory, sizeof(memory)) && memory.State == MEM_COMMIT
               ? memory.RegionSize
               : 0;
}
void emit(const TraceRecord& record) noexcept {
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), record.text, record.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
void memory_stage(const char* stage) noexcept {
    const auto* p = g_app.gif_favorites;
    // Each of these is a separately owned allocation. Query once per bank;
    // no double counting of borrowed group/record/index pointers.
    const usize owner = committed(p);
    const usize bank = committed(p ? p->bank : nullptr);
    const usize queue = committed(p ? p->queue : nullptr);
    const usize response_bank = committed(p ? p->response_body : nullptr);
    TraceRecord record("gif_favorites_runtime_memory");
    record.field("stage", stage);
    record.field("logical_bytes", application_gif_favorites_owner_bytes());
    record.field("committed_bytes", owner + bank + queue + response_bank);
    record.field("owner_committed", owner);
    record.field("confirmed_bank_committed", bank);
    record.field("queue_committed", queue);
    record.field("response_committed", response_bank);
    emit(record);
}
struct Binary {
    u8 bytes[8192];
    u32 used;
    void number(u64 value) noexcept {
        do {
            u8 part = static_cast<u8>(value & 127);
            value >>= 7;
            bytes[used++] = part | (value ? 128 : 0);
        } while (value);
    }
    void scalar(u32 field, u64 value) noexcept {
        number(field * 8);
        number(value);
    }
    void raw(ConstSlice<u8> value) noexcept {
        copy_bytes(bytes + used, value.data, value.count);
        used += value.count;
    }
    void message(u32 field, ConstSlice<u8> value) noexcept {
        number(field * 8 + 2);
        number(value.count);
        raw(value);
    }
    void text(u32 field, Utf8View value) noexcept {
        message(field, {reinterpret_cast<const u8*>(value.data), value.bytes});
    }
    ConstSlice<u8> view() const noexcept {
        return {bytes, used};
    }
};
void add(Binary& group, Utf8View key, u32 order, bool unknown = false) noexcept {
    Binary value{}, entry{};
    value.scalar(1, 1);
    value.text(2, source);
    value.scalar(3, 160);
    value.scalar(4, 90);
    value.scalar(5, order);
    if (unknown)
        value.scalar(88, 11);
    entry.text(1, key);
    entry.message(2, value.view());
    if (unknown)
        entry.scalar(77, 9);
    group.message(1, entry.view());
}
Binary proto(u32 version, bool first = true, bool second = false) noexcept {
    Binary versions{}, group{}, root{};
    versions.scalar(1, 1);
    versions.scalar(2, 1);
    versions.scalar(3, version);
    root.message(1, versions.view());
    if (first)
        add(group, saved, 1, true);
    if (second)
        add(group, another, 2);
    group.scalar(99, 7);
    root.message(2, group.view());
    return root;
}
u8 body_storage[32768];
Utf8View envelope(ConstSlice<u8> input, bool conflict = false, bool escaped = false) noexcept {
    char encoded[12000]{};
    const auto b64 = base64_encode(input, {encoded, sizeof(encoded)});
    check(b64.ok(), "fixture base64 encoding succeeds");
    ProtocolWriter writer{{body_storage, sizeof(body_storage)}, 0};
    protocol_writer_append_literal(&writer, "{\"settings\":\"");
    for (u32 i = 0; i < b64.bytes; ++i) {
        if (escaped && i == 0) {
            protocol_writer_append_literal(&writer, "\\u00");
            const char hex[]{"0123456789abcdef"[static_cast<u8>(encoded[i]) >> 4],
                             "0123456789abcdef"[static_cast<u8>(encoded[i]) & 15]};
            protocol_writer_append_bytes(&writer, {reinterpret_cast<const u8*>(hex), 2});
        } else
            protocol_writer_append_bytes(&writer, {reinterpret_cast<const u8*>(encoded + i), 1});
    }
    if (conflict)
        protocol_writer_append_literal(&writer, "\",\"out_of_date\":true}");
    else
        protocol_writer_append_literal(&writer, "\",\"out_of_date\":false}");
    return {reinterpret_cast<const char*>(body_storage), writer.used};
}
RestClientResponse response(u64 id, u16 code = 200) noexcept {
    RestClientResponse result{};
    result.request_id = id;
    result.status_code = code;
    return result;
}
bool complete(const RestClientResponse& result, Utf8View body) noexcept {
    const auto buffer = application_gif_favorites_response_buffer();
    if (body.bytes)
        copy_bytes(buffer.data, body.data, body.bytes);
    const bool accepted = application_gif_favorites_response(
        result, {reinterpret_cast<const char*>(buffer.data), body.bytes});
    // Match Main's erase guard: erase first, then release the temporary bank.
    if (body.bytes)
        SecureZeroMemory(buffer.data, body.bytes);
    application_gif_favorites_response_drained();
    return accepted;
}
RestClientSubmitResult WINAPI submit(void*, AccountGifFavoritesOperation operation, ConstSlice<u8> patch,
                                     u64 id) noexcept {
    if (operation == AccountGifFavoritesOperation::cancel) {
        ++cancels;
        cancelled_id = id;
        return {};
    }
    if (admission != RestClientSubmitError::none)
        return {admission};
    ++submits;
    submitted_id = id;
    submitted_operation = operation;
    captured_bytes = patch.count;
    check(patch.count <= sizeof(captured_patch),
          "submitted conditional envelope fits complete bounded capture");
    if (patch.count <= sizeof(captured_patch))
        copy_bytes(captured_patch, patch.data, patch.count);
    if (operation == AccountGifFavoritesOperation::update) {
        MEMORY_BASIC_INFORMATION allocation{};
        check(VirtualQuery(patch.data, &allocation, sizeof(allocation)) && allocation.State == MEM_COMMIT,
              "conditional write holds owned transient storage during synchronous admission");
        transient_base = allocation.AllocationBase;
        transient_bytes = allocation.RegionSize;
        if (id == 102) {
            TraceRecord record("gif_favorites_runtime_patch");
            record.field("stage", "synchronous_submit");
            record.field("json_bytes", patch.count);
            record.field("transient_committed_bytes", transient_bytes);
            emit(record);
        }
    }
    check((operation == AccountGifFavoritesOperation::fetch && !patch.count) ||
              (operation == AccountGifFavoritesOperation::update &&
               gif_favorites_validate_patch_json(
                   {reinterpret_cast<const char*>(patch.data), patch.count}) == GifFavoritesError::none),
          "account helper receives only fetch or codec validated favorite group conditional patch");
    return {};
}
struct PatchEnvelope {
    JsonString settings;
    u64 version;
    bool found;
};
bool patch_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<PatchEnvelope*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("settings"))) {
        e.found = json_value_as_string(value, &e.settings);
        return e.found;
    }
    if (json_string_equals_ascii(name, utf8_literal("required_data_version")))
        return json_value_to_u64(value, &e.version);
    return false;
}
Binary patch_proto(u32* required) noexcept {
    u32 indexes[64]{};
    const Utf8View text{reinterpret_cast<const char*>(captured_patch), captured_bytes};
    const auto scanned = json_scan_structurals_avx1(text, {indexes, 64});
    PatchEnvelope e{};
    check(scanned.ok() &&
              json_visit_root_object(text, {indexes, scanned.count}, 8, patch_member, &e).ok() && e.found,
          "captured update has complete settings plus required version envelope");
    Binary result{};
    const auto decoded = base64_decode(e.settings.encoded, {result.bytes, sizeof(result.bytes)});
    check(decoded.ok(), "captured settings decode completely");
    result.used = decoded.bytes;
    *required = static_cast<u32>(e.version);
    return result;
}
Utf8View reply_patch(u32 version, bool conflict = false) noexcept {
    u32 required{};
    const auto patch = patch_proto(&required);
    Binary root{}, versions{};
    versions.scalar(1, 1);
    versions.scalar(2, 1);
    versions.scalar(3, version);
    root.message(1, versions.view());
    root.raw(patch.view());
    return envelope(root.view(), conflict);
}
bool contains(ConstSlice<u8> bytes, ConstSlice<u8> pattern) noexcept {
    for (u32 i = 0; i + pattern.count <= bytes.count; ++i)
        if (bytes_equal(bytes.data + i, pattern.data, pattern.count))
            return true;
    return false;
}
void configure() noexcept {
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.api.gif_favorites = submit;
    g_app.account.status.profile_id = 4;
    g_app.account.status.running = true;
    g_app.account.status.frecency_settings_revision = 1;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "synthetic account identity allocation succeeds");
    if (g_app.account_ui)
        g_app.account_ui->chat_state.self_user_id = 7;
}
void load(u64 id, u32 version, bool first = true, bool second = false, bool escaped = false) noexcept {
    if (g_app.gif_favorites && g_app.gif_favorites->failed && g_app.gif_favorites->visible)
        application_gif_favorites_set_visible(false); // explicit close/reopen retry
    const bool opened = application_gif_favorites_open();
    if (id == 101)
        memory_stage("open");
    check(opened && application_gif_favorites_pending(1) && application_gif_favorites_submit(id, 1),
          "lazy visible Favorites submits only through existing account primary API");
    const auto fixture = proto(version, first, second);
    check(complete(response(id), envelope(fixture.view(), false, escaped)) &&
              application_gif_favorites_view().authoritative,
          "complete bounded snapshot publishes authoritative saved data");
}
void run() noexcept {
    configure();
    if (!g_app.account_ui)
        return;
    TraceRecord sizes("gif_favorites_runtime_sizes");
    sizes.field("sizeof_owner", sizeof(ApplicationGifFavorites));
    sizes.field("sizeof_confirmed_bank", sizeof(FavoriteBank));
    sizes.field("sizeof_action", sizeof(FavoriteAction));
    emit(sizes);
    memory_stage("dormant");
    for (u32 i = 0; i < 8; ++i) {
        application_gif_favorites_synchronize();
        application_gif_favorites_pending(1);
        application_gif_favorites_view();
        application_gif_favorites_state(saved);
    }
    check(!g_app.gif_favorites && !application_gif_favorites_owner_bytes() && !submits &&
              !application_gif_favorites_response_buffer().data && !application_gif_favorites_next_wake(),
          "dormant broker commits zero owner banks and performs zero account work");
    load(101, 10, true, false, true);
    memory_stage("loaded1");
    const auto original = application_gif_favorites_view();
    check(original.records.count == 1 && equal(original.records[0].url, saved) &&
              application_gif_favorites_state(saved).confirmed &&
              !application_gif_favorites_state(another).confirmed,
          "escaped JSON settings decode owns exact sorted record URLs and membership accelerator");
    const u32 stable_callbacks = callbacks, stable_submits = submits;
    for (u32 i = 0; i < 8; ++i) {
        application_gif_favorites_open();
        application_gif_favorites_set_visible(true);
    }
    check(callbacks == stable_callbacks && submits == stable_submits &&
              application_gif_favorites_view().revision == original.revision &&
              !application_gif_favorites_next_wake(),
          "repeated visible open is idempotent with no revision invalidation frame callbacks or network "
          "wake");
    const auto identical_bank = g_app.gif_favorites->bank;
    ++g_app.account.status.frecency_settings_revision;
    application_gif_favorites_synchronize();
    const auto identical_snapshot = proto(10);
    check(application_gif_favorites_submit(1000, 1) &&
              complete(response(1000), envelope(identical_snapshot.view())) &&
              g_app.gif_favorites->bank == identical_bank &&
              application_gif_favorites_view().data_revision == original.data_revision,
          "identical refreshed group preserves immutable record borrows and preview data revision");
    check(application_gif_favorites_toggle(another, source, 160, 90, GifFavoriteFormat::image),
          "explicit favorite mutation is queued");
    memory_stage("pendingqueued");
    const auto queued = application_gif_favorites_view();
    check(queued.data_revision == original.data_revision && queued.revision != original.revision &&
              !application_gif_favorites_state(another).confirmed &&
              application_gif_favorites_state(another).pending &&
              application_gif_favorites_state(saved).confirmed,
          "pending state changes keep confirmed bank and preview data revision immutable");
    check(!application_gif_favorites_toggle(another, source, 160, 90, GifFavoriteFormat::image),
          "same URL cannot enqueue conflicting actions while pending");
    check(application_gif_favorites_submit(102, 1) &&
              submitted_operation == AccountGifFavoritesOperation::update,
          "conditional favorite update serializes through primary API");
    memory_stage("submitted_response");
    check(
        transient_base && transient_bytes < 16 * 1024 && is_free(transient_base),
        "tiny mutation commits below 16 KiB transient scratch and JSON and releases both after admission");
    TraceRecord transient_released("gif_favorites_runtime_patch");
    transient_released.field("stage", "after_submit");
    transient_released.field("transient_committed_bytes", committed(transient_base));
    transient_released.field("freed", is_free(transient_base) ? 1ull : 0ull);
    emit(transient_released);
    u32 required{};
    const auto patch = patch_proto(&required);
    constexpr u8 group_unknown[]{0x98, 0x06, 0x07}, entry_unknown[]{0xe8, 0x04, 0x09},
        value_unknown[]{0xc0, 0x05, 0x0b};
    check(
        required == 10 && contains(patch.view(), {group_unknown, sizeof(group_unknown)}) &&
            contains(patch.view(), {entry_unknown, sizeof(entry_unknown)}) &&
            contains(patch.view(), {value_unknown, sizeof(value_unknown)}),
        "write uses last authoritative dataVersion and preserves every opaque group entry and value field");
    const auto held = application_gif_favorites_response_buffer();
    check(held.data && held.count == 2 * 1024 * 1024,
          "exact ticket owns temporary 2MiB take buffer before submission");
    check(!application_gif_favorites_response(response(999), {}) &&
              application_gif_favorites_response_buffer().data == held.data,
          "unrelated receipt leaves exact Favorites ticket and take buffer unchanged");
    application_gif_favorites_response_drained();
    check(application_gif_favorites_response_buffer().data == held.data,
          "details erase drain cannot release still pending primary take buffer");
    const auto updated = reply_patch(11);
    copy_bytes(held.data, updated.data, updated.bytes);
    watch_bank = g_app.gif_favorites->bank;
    callback_before_free = false;
    const auto old_bank = watch_bank;
    check(application_gif_favorites_response(response(102),
                                             {reinterpret_cast<const char*>(held.data), updated.bytes}) &&
              application_gif_favorites_state(another).confirmed &&
              !application_gif_favorites_state(another).pending &&
              application_gif_favorites_view().data_revision != original.data_revision &&
              callback_before_free && is_free(old_bank) &&
              application_gif_favorites_response_buffer().data == held.data &&
              !application_gif_favorites_pending(1),
          "verified result swaps immutable bank with callback before borrowed old bank free and defers "
          "take buffer drain");
    watch_bank = nullptr;
    SecureZeroMemory(held.data, updated.bytes);
    application_gif_favorites_response_drained();
    check(is_free(held.data) && !application_gif_favorites_response_buffer().data,
          "take buffer remains writable for RAII erase then is freed by exact response drain");
    memory_stage("confirmed_drained");
    const auto baseline_bytes = application_gif_favorites_owner_bytes();
    check(application_gif_favorites_toggle(saved, source, 160, 90, GifFavoriteFormat::image) &&
              application_gif_favorites_submit(103, 1),
          "confirmed removal is pending");
    application_gif_favorites_set_visible(false);
    check(application_gif_favorites_state(saved).confirmed && g_app.gif_favorites->queue_count == 1 &&
              g_app.gif_favorites->request_id == 103 && !cancels,
          "closing gallery preserves account owned explicit mutation and confirmed star");
    auto failed = response(103);
    failed.error = RestClientError::timeout;
    check(complete(failed, {}) && application_gif_favorites_state(saved).confirmed &&
              application_gif_favorites_state(saved).failed &&
              !application_gif_favorites_state(saved).pending &&
              !application_gif_favorites_pending(~u64{0}) && !application_gif_favorites_next_wake(),
          "ambiguous mutation failure preserves confirmed star and never blindly replays action");
    load(104, 11, true, true);
    check(application_gif_favorites_toggle(saved, source, 160, 90, GifFavoriteFormat::image) &&
              application_gif_favorites_toggle(third, source, 100, 100, GifFavoriteFormat::image) &&
              application_gif_favorites_submit(105, 1),
          "distinct queued actions serialize");
    const auto conflict_snapshot = proto(20, true, true);
    check(
        complete(response(105), envelope(conflict_snapshot.view(), true)) &&
            application_gif_favorites_state(saved).confirmed &&
            application_gif_favorites_state(saved).failed &&
            !application_gif_favorites_state(saved).pending &&
            application_gif_favorites_state(third).pending && application_gif_favorites_view().failed &&
            application_gif_favorites_view().status.bytes,
        "out of date publishes returned authority discards conflicting action and requires explicit retry");
    check(application_gif_favorites_submit(106, 1),
          "other queued action rebases on returned authoritative conflict snapshot");
    patch_proto(&required);
    check(required == 20, "distinct queued update uses newly returned dataVersion");
    auto limited = response(106, 429);
    limited.rate_limited = true;
    limited.retry_at_ms = clock_monotonic_milliseconds() + 1000;
    check(complete(limited, {}) && application_gif_favorites_next_wake() == limited.retry_at_ms &&
              !application_gif_favorites_pending(limited.retry_at_ms - 1) &&
              application_gif_favorites_submit(107, limited.retry_at_ms),
          "server rejected rate limit retry respects exact bounded deadline");
    check(complete(response(107), reply_patch(21)) && application_gif_favorites_state(third).confirmed,
          "verified rebased queued action completes after bounded rate retry");
    // Gateway type2/READY/RESUMED revision invalidates admission without timers.
    application_gif_favorites_set_visible(false);
    ++g_app.account.status.frecency_settings_revision;
    application_gif_favorites_synchronize();
    check(!application_gif_favorites_pending(~u64{0}) && !application_gif_favorites_next_wake(),
          "hidden Gateway settings update performs no idle network poll");
    check(application_gif_favorites_open() && application_gif_favorites_submit(108, 1),
          "visible invalidated cache lazily reloads");
    const auto cancelled_buffer = application_gif_favorites_response_buffer();
    application_gif_favorites_set_visible(false);
    check(cancels == 1 && cancelled_id == 108 &&
              application_gif_favorites_response_buffer().data == cancelled_buffer.data,
          "closing fetch retires admission and cancels exact ticket retaining output until drain");
    auto cancelled = response(108);
    cancelled.error = RestClientError::cancelled;
    check(complete(cancelled, {}) && !application_gif_favorites_pending(~u64{0}) &&
              is_free(cancelled_buffer.data),
          "cancelled closed fetch drains without publishing stale snapshot or restarting admission");
    load(109, 30, true, false);
    check(application_gif_favorites_toggle(another, source, 160, 90, GifFavoriteFormat::image) &&
              application_gif_favorites_submit(110, 1),
          "source generation fixture submits mutation");
    ++g_app.account.status.frecency_settings_revision;
    application_gif_favorites_synchronize();
    check(complete(response(110), reply_patch(31)) && !application_gif_favorites_state(another).confirmed &&
              !application_gif_favorites_state(another).pending &&
              application_gif_favorites_state(another).failed && application_gif_favorites_pending(1),
          "Gateway source generation change rejects stale write publication and action replay");
    check(application_gif_favorites_submit(111, 1),
          "stale write requires fresh snapshot before next action");
    const auto before_switch = application_gif_favorites_response_buffer();
    ++g_app.account.status.profile_id;
    ++g_app.account_ui->chat_state.self_user_id;
    application_gif_favorites_synchronize();
    check(!application_gif_favorites_view().records.count &&
              !application_gif_favorites_state(saved).confirmed &&
              application_gif_favorites_response_buffer().data == before_switch.data,
          "account scope switch invalidates borrowed confirmed data retaining exact old take receipt");
    const auto foreign = proto(99, true, true);
    check(complete(response(111), envelope(foreign.view())) &&
              !application_gif_favorites_view().records.count && is_free(before_switch.data) &&
              g_app.gif_favorites->profile_id == g_app.account.status.profile_id,
          "old profile self completion drains without publishing into new scope");
    application_gif_favorites_release();
    memory_stage("released");
    // Queue allocation is lazy and bounded, and it copies caller strings.
    load(112, 40, false, false);
    const auto queue_baseline = application_gif_favorites_owner_bytes();
    char urls[8][64]{};
    for (u32 i = 0; i < 8; ++i) {
        ProtocolWriter writer{{reinterpret_cast<u8*>(urls[i]), sizeof(urls[i])}, 0};
        protocol_writer_append_literal(&writer, "https://example.test/queued-");
        protocol_writer_append_u64(&writer, i);
        check(application_gif_favorites_toggle({urls[i], writer.used}, source, 90, 90,
                                               GifFavoriteFormat::image),
              "bounded queue admits each distinct explicit action");
        urls[i][0] = 'X';
    }
    check(g_app.gif_favorites->queue_count == 8 && g_app.gif_favorites->queue[0].url[0] == 'h' &&
              application_gif_favorites_owner_bytes() ==
                  queue_baseline + kQueueCapacity * sizeof(FavoriteAction) &&
              !application_gif_favorites_toggle(third, source, 90, 90, GifFavoriteFormat::image) &&
              g_app.gif_favorites->queue_count == 8,
          "queue copies exact action strings allocates only lazily and rejects ninth without truncation");
    admission = RestClientSubmitError::busy;
    check(!application_gif_favorites_submit(113, 1) && !application_gif_favorites_response_buffer().data &&
              !application_gif_favorites_next_wake(),
          "busy primary admission frees take buffer and cannot spin on zero deadline");
    admission = RestClientSubmitError::none;
    check(application_gif_favorites_submit(114, 1),
          "primary lane completion can retry busy admission normally");
    // Joined-account release may retire exact pending receipt directly.
    const auto teardown_buffer = application_gif_favorites_response_buffer();
    const void* queue = g_app.gif_favorites->queue;
    const void* bank = g_app.gif_favorites->bank;
    const void* owner = g_app.gif_favorites;
    application_gif_favorites_release();
    check(!g_app.gif_favorites && !application_gif_favorites_owner_bytes() &&
              is_free(teardown_buffer.data) && is_free(queue) && is_free(bank) && is_free(owner),
          "joined teardown releases all owner queue confirmed and pending response commitments");
    load(115, 50, true, false);
    check(application_gif_favorites_toggle(third, source, 90, 90, GifFavoriteFormat::image) &&
              application_gif_favorites_submit(116, 1),
          "bounded rate retry fixture starts explicit action");
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        auto rejected = response(116 + attempt, 429);
        rejected.rate_limited = true;
        rejected.retry_at_ms = clock_monotonic_milliseconds() + 1000;
        check(complete(rejected, {}), "bounded rate receipt drains exact rejected attempt");
        if (attempt < 2)
            check(application_gif_favorites_submit(117 + attempt, rejected.retry_at_ms),
                  "rejected rate retry is admitted only within three attempt bound");
    }
    check(!application_gif_favorites_pending(~u64{0}) && !application_gif_favorites_state(third).pending &&
              application_gif_favorites_state(third).failed &&
              application_gif_favorites_state(saved).confirmed,
          "third rejected rate receipt stops retries preserving confirmed authority display");
    const auto failed_callbacks = callbacks;
    application_gif_favorites_open();
    application_gif_favorites_set_visible(true);
    check(callbacks == failed_callbacks && !application_gif_favorites_pending(~u64{0}) &&
              !application_gif_favorites_next_wake(),
          "repeated open after failure cannot restart an idle retry loop");
    load(200, 60, false, false);
    static char boundary_urls[4][kMediaCatalogMaximumUrlBytes + 1];
    static char boundary_source[kMediaCatalogMaximumUrlBytes];
    memset(boundary_source, 's', sizeof(boundary_source));
    constexpr char source_prefix[] = "https://example.test/";
    copy_bytes(boundary_source, source_prefix, sizeof(source_prefix) - 1);
    constexpr u32 lengths[]{2048, 1024, 1025, 2049};
    for (u32 i = 0; i < 4; ++i) {
        memset(boundary_urls[i], 'a' + i, lengths[i]);
        constexpr char prefix[] = "https://example.test/";
        copy_bytes(boundary_urls[i], prefix, sizeof(prefix) - 1);
        const Utf8View action_source = i ? source : Utf8View{boundary_source, sizeof(boundary_source)};
        const bool accepted = application_gif_favorites_toggle(
            {boundary_urls[i], lengths[i]}, action_source, 90, 90, GifFavoriteFormat::image);
        check(accepted == (i < 3),
              "catalog URL bounds accept 1024 1025 2048 bytes and reject 2049 without truncation");
        if (i < 3) {
            check(g_app.gif_favorites->queue[i].url_bytes == lengths[i] &&
                      bytes_equal(g_app.gif_favorites->queue[i].url, boundary_urls[i], lengths[i]),
                  "catalog length boundary action owns every exact URL byte");
            boundary_urls[i][0] = 'X';
        }
    }
    check(g_app.gif_favorites->queue_count == 3 && g_app.gif_favorites->queue[0].url[0] == 'h' &&
              g_app.gif_favorites->queue[0].src_bytes == 2048 && application_gif_favorites_submit(201, 1),
          "full catalog 2048 byte key survives copied queue and bounded patch planning");
    const auto long_patch = patch_proto(&required);
    check(required == 60 && gif_favorites_measure_proto(long_patch.view()).ok() && is_free(transient_base),
          "full catalog key emits complete valid protobuf and releases derived transient storage");
    application_gif_favorites_release();
    // A fetch required by an explicit queued change is account-owned, even if
    // the gallery that originated the action closes before authority arrives.
    check(application_gif_favorites_open() &&
              application_gif_favorites_toggle(another, source, 160, 90, GifFavoriteFormat::image) &&
              application_gif_favorites_submit(202, 1),
          "queued change fetch fixture begins before first authoritative snapshot");
    const auto fetch_generation = g_app.gif_favorites->generation;
    const auto fetch_buffer = application_gif_favorites_response_buffer();
    const auto cancellations_before_close = cancels;
    application_gif_favorites_set_visible(false);
    check(cancels == cancellations_before_close && g_app.gif_favorites->generation == fetch_generation &&
              g_app.gif_favorites->request_id == 202 &&
              application_gif_favorites_response_buffer().data == fetch_buffer.data,
          "closing preserves exact authority fetch required by account owned queued action without cancel "
          "and refetch");
    const auto empty_authority = proto(70, false, false);
    check(complete(response(202), envelope(empty_authority.view())) &&
              application_gif_favorites_pending(1) && application_gif_favorites_submit(203, 1) &&
              submitted_operation == AccountGifFavoritesOperation::update,
          "closed queued action proceeds from confirmed authority through the existing primary lane");
    check(complete(response(203), reply_patch(71)) && application_gif_favorites_state(another).confirmed &&
              !application_gif_favorites_pending(~u64{0}) && !application_gif_favorites_next_wake(),
          "closed queued mutation confirms once and returns to dormant scheduling");
    application_gif_favorites_release();
    check(baseline_bytes > sizeof(ApplicationGifFavorites),
          "authoritative cache owns measured record group and index commitment");
    dispose(g_app.account_ui, sizeof(AccountUiRuntime));
    g_app.account_ui = nullptr;
    g_app.account = {};
}
} // namespace maicord::gif_favorites_runtime_test

namespace maicord::app_detail {
void application_media_picker_favorites_changed() noexcept {
    ++gif_favorites_runtime_test::callbacks;
    if (gif_favorites_runtime_test::watch_bank) {
        MEMORY_BASIC_INFORMATION memory{};
        gif_favorites_runtime_test::callback_before_free |=
            VirtualQuery(gif_favorites_runtime_test::watch_bank, &memory, sizeof(memory)) &&
            memory.State == MEM_COMMIT &&
            (!g_app.gif_favorites || g_app.gif_favorites->bank != gif_favorites_runtime_test::watch_bank);
    }
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_application_gif_favorites_runtime_entry() noexcept {
    maicord::gif_favorites_runtime_test::run();
    const bool failed = maicord::gif_favorites_runtime_test::failures != 0;
    constexpr char pass[] = "PASS application GIF Favorites runtime\n",
                   fail[] = "FAIL application GIF Favorites runtime\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1u : 0u);
}

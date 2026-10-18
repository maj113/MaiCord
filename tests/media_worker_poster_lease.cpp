// Local video poster lease: real media-worker threads and cleanup APIs. Fake COM
// objects control only the fence result and notification; the sparse source is
// a real allocation. Wall-clock limits below only bound hangs.
#include "maicord/app/account_media_worker.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord { namespace {
u64 deadline{};
void fail(u32 line) noexcept {
    TraceRecord out("poster_lease_failure"); out.field("line", line);
    DWORD written{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.text, out.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}
#define CHECK(value) do { if (!(value)) { fail(__LINE__); return false; } } while (false)
enum class Failure : u8 { none, context, device, create, signal, registration };
struct Fake;
struct Object { void** table; Fake* owner; volatile LONG references; u32 kind; };
struct Fake {
    // Windows SDK COM slot order, verified against d3d11_4.h/d3d11_3.h.
    void* device_table[69]{}, *context_table[149]{}, *fence_table[10]{};
    Object device{}, context{}, fence{};
    AccountMediaWorker* worker = nullptr;
    HANDLE event = nullptr;
    Failure failure = Failure::none;
    bool immediate = false;
    volatile LONG in_registration = 0, early_release = 0, registrations = 0, signals = 0, releases = 0;
    DWORD last_release_thread = 0;
};
ULONG STDMETHODCALLTYPE add_ref(Object* object) noexcept { return static_cast<ULONG>(InterlockedIncrement(&object->references)); }
ULONG STDMETHODCALLTYPE release(Object* object) noexcept {
    auto& fake = *object->owner;
    if (object->kind != 0 && ReadAcquire(&fake.in_registration)) InterlockedIncrement(&fake.early_release);
    if (object->kind == 2) { fake.last_release_thread = GetCurrentThreadId(); InterlockedIncrement(&fake.releases); }
    return static_cast<ULONG>(InterlockedDecrement(&object->references));
}
HRESULT STDMETHODCALLTYPE query(Object* object, REFIID iid, void** out) noexcept {
    if (!out) return E_POINTER; *out = nullptr;
    auto& fake = *object->owner;
    if ((object->kind == 0 && fake.failure == Failure::device) ||
        (object->kind == 1 && fake.failure == Failure::context)) return E_NOINTERFACE;
    if (iid == __uuidof(IUnknown) || (object->kind == 0 && iid == __uuidof(ID3D11Device5)) ||
        (object->kind == 1 && iid == __uuidof(ID3D11DeviceContext4)) ||
        (object->kind == 2 && iid == __uuidof(ID3D11Fence))) {
        add_ref(object); *out = object; return S_OK;
    }
    return E_NOINTERFACE;
}
void STDMETHODCALLTYPE immediate_context(Object* object, ID3D11DeviceContext** out) noexcept {
    add_ref(&object->owner->context); *out = reinterpret_cast<ID3D11DeviceContext*>(&object->owner->context);
}
HRESULT STDMETHODCALLTYPE create_fence(Object* object, UINT64 initial, D3D11_FENCE_FLAG, REFIID iid, void** out) noexcept {
    if (!out) return E_POINTER; *out = nullptr;
    if (object->owner->failure == Failure::create) return E_OUTOFMEMORY;
    if (initial || iid != __uuidof(ID3D11Fence)) return E_INVALIDARG;
    add_ref(&object->owner->fence); *out = &object->owner->fence; return S_OK;
}
HRESULT STDMETHODCALLTYPE signal(Object* object, ID3D11Fence* fence, UINT64 value) noexcept {
    auto& fake = *object->owner; InterlockedIncrement(&fake.signals);
    if (fake.failure == Failure::signal) return E_FAIL;
    return fence == reinterpret_cast<ID3D11Fence*>(&fake.fence) && value == 1 ? S_OK : E_INVALIDARG;
}
HRESULT STDMETHODCALLTYPE register_event(Object* object, UINT64 value, HANDLE event) noexcept {
    auto& fake = *object->owner; InterlockedIncrement(&fake.registrations);
    if (fake.failure == Failure::registration) return E_FAIL;
    if (value != 1 || !event) return E_INVALIDARG;
    fake.event = event;
    if (fake.immediate) {
        InterlockedExchange(&fake.in_registration, 1);
        const BOOL signaled = SetEvent(event);
        // Give the actual worker a chance to reach the registration handoff.
        Sleep(2);
        if (!signaled) { InterlockedExchange(&fake.in_registration, 0); return E_FAIL; }
        InterlockedExchange(&fake.in_registration, 0);
    }
    return S_OK;
}
void initialize_fake(Fake& fake) noexcept {
    fake.device = {fake.device_table, &fake, 1, 0};
    fake.context = {fake.context_table, &fake, 1, 1};
    fake.fence = {fake.fence_table, &fake, 1, 2};
    void** tables[]{fake.device_table, fake.context_table, fake.fence_table};
    for (auto* table : tables) {
        table[0] = reinterpret_cast<void*>(&query); table[1] = reinterpret_cast<void*>(&add_ref);
        table[2] = reinterpret_cast<void*>(&release);
    }
    fake.device_table[40] = reinterpret_cast<void*>(&immediate_context);
    fake.device_table[68] = reinterpret_cast<void*>(&create_fence);
    fake.context_table[147] = reinterpret_cast<void*>(&signal);
    fake.fence_table[9] = reinterpret_cast<void*>(&register_event);
}
struct Harness {
    Fake fake{};
    AccountMediaWorker* worker = nullptr;
    ~Harness() noexcept {
        if (!worker) return;
        if (worker->initialized) {
            if (fake.event) SetEvent(fake.event);
            const u64 now = GetTickCount64();
            if (!account_media_worker_stop(worker, static_cast<u32>(now < deadline ? deadline - now : 1))) ExitProcess(2);
        }
        VirtualFree(worker, 0, MEM_RELEASE);
    }
};
bool begin(Harness& h) noexcept {
    if (!h.worker) {
        void* allocation = VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(allocation); h.worker = new (allocation) AccountMediaWorker;
        initialize_fake(h.fake); h.fake.worker = h.worker;
    }
    CHECK(account_media_worker_initialize(h.worker).ok());
    return true;
}
AccountMediaWorkerPhase phase(const AccountMediaWorkerLane& lane) noexcept {
    return static_cast<AccountMediaWorkerPhase>(ReadAcquire(&lane.phase));
}
GpuVideoFrame gpu{};
void retain_source(void*) noexcept {}
void release_source(void*) noexcept {}
ConstSlice<u8> no_read(void*, u64, Slice<u8>) noexcept { return {}; }
const UploadSourceOps source_ops{retain_source, release_source, no_read};
bool seed(Harness& h, u32 index, u64 serial, AccountMediaLocalPreviewRequest* token, const u8** sparse) noexcept {
    auto& lane = h.worker->lanes[index]; CHECK(phase(lane) == AccountMediaWorkerPhase::idle);
    // A lane that completed a lease has started (lanes start on first command).
    CHECK(account_media_worker_start_lane(h.worker, index).ok() && lane.thread);
    // Seed only a quiescent completed lease. All transitions and final resource
    // release below run in production APIs and production worker threads.
    lane.command = {}; lane.completed = {}; lane.completed_result = {};
    lane.command.kind = AccountMediaWorkKind::local_draft_preview;
    lane.command.key = media_asset_local_draft_preview(9000 + index, kLocalDraftPreviewThumbnailVariant | 1,
                                                      MediaAttachmentFormat::mp4);
    lane.command.catalog_scope = 700; lane.command.catalog_generation = 8;
    lane.command.lease_serial = serial; lane.command.video_device = reinterpret_cast<ID3D11Device*>(&h.fake.device);
    lane.command.local_content = {&h, &source_ops, 1};
    lane.command.local_snapshot = {{lane.command.key.id, 19, 3, 0}, 1};
    lane.completed = lane.command; lane.completed_result.state = AccountMediaRequestState::ready;
    lane.media.mp4_key = lane.command.key; lane.media.frame_kind = AccountMediaFrameKind::video_nv12_gpu;
    lane.media.mp4_decoder = &lane.media.owned_mp4_decoder;
    lane.media.owned_mp4_decoder = {}; lane.media.owned_mp4_decoder.ready = true; lane.media.owned_mp4_decoder.gpu = &gpu;
    void* allocation = VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(allocation); lane.media.progressive = new (allocation) ProgressiveVideoFetch;
    CHECK(progressive_source_initialize(&lane.media.progressive->source, 4096));
    static const u8 bytes[4096]{1};
    CHECK(progressive_source_write(&lane.media.progressive->source, 0, {bytes, sizeof(bytes)}));
    *sparse = lane.media.progressive->source.base;
    InterlockedExchange(&lane.local_retired, 0);
    *token = {}; token->key = lane.command.key; token->owner_scope = 700; token->owner_generation = 8; token->lease_serial = serial;
    InterlockedExchange(&lane.phase, static_cast<LONG>(AccountMediaWorkerPhase::leased));
    return true;
}
bool idle(Harness& h, u32 index, const u8* sparse) noexcept {
    auto& lane = h.worker->lanes[index];
    while (phase(lane) != AccountMediaWorkerPhase::idle) { CHECK(GetTickCount64() < deadline); Sleep(1); }
    CHECK(!lane.media.progressive && !lane.media.owned_mp4_decoder.ready && !lane.poster_fence && !lane.poster_context);
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(sparse, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE);
    return true;
}
bool cancel(Harness& h, AccountMediaLocalPreviewRequest token) noexcept {
    token.lease_serial = 0;
    return account_media_worker_retire_local_preview(h.worker, token).state == AccountMediaRequestState::ready;
}
bool uploaded(Harness& h, const AccountMediaLocalPreviewRequest& token) noexcept {
    return account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::pending;
}
bool arm(Harness& h) noexcept {
    CHECK(account_media_worker_local_previews_presented(h.worker).state == AccountMediaRequestState::ready);
    return true;
}
bool no_refs(Harness& h) noexcept {
    CHECK(h.fake.device.references == 1 && h.fake.context.references == 1 && h.fake.fence.references == 1);
    CHECK(h.fake.early_release == 0); return true;
}
bool ordinary_cases(Harness& h) noexcept {
    AccountMediaLocalPreviewRequest token{}; const u8* sparse{};
    CHECK(seed(h, 0, 1, &token, &sparse));
    auto bad = token; ++bad.lease_serial;
    CHECK(account_media_worker_local_preview_uploaded(h.worker, bad).state == AccountMediaRequestState::invalid);
    bad = token; bad.video_device = reinterpret_cast<ID3D11Device*>(&h.fake.device);
    CHECK(account_media_worker_local_preview_uploaded(h.worker, bad).state == AccountMediaRequestState::invalid);
    auto& lane = h.worker->lanes[0];
    InterlockedExchange(&lane.phase, static_cast<LONG>(AccountMediaWorkerPhase::working));
    CHECK(account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::invalid);
    InterlockedExchange(&lane.phase, static_cast<LONG>(AccountMediaWorkerPhase::selected_video));
    CHECK(account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::invalid);
    InterlockedExchange(&lane.phase, static_cast<LONG>(AccountMediaWorkerPhase::leased));
    CHECK(uploaded(h, token) && uploaded(h, token));
    AccountMediaLocalPreviewRequest same{};
    same.content = lane.command.local_content; same.snapshot = lane.command.local_snapshot;
    same.key = token.key; same.owner_scope = token.owner_scope; same.owner_generation = token.owner_generation;
    same.video_device = lane.command.video_device;
    const LONG64 queued = h.worker->counters.queued;
    CHECK(account_media_worker_request_local_preview(h.worker, same, 1, 1).state == AccountMediaRequestState::pending);
    CHECK(h.worker->counters.queued == queued && lane.command.lease_serial == token.lease_serial);
    // A hidden/skipped Present invokes no presented API and arms no event.
    CHECK(phase(lane) == AccountMediaWorkerPhase::poster_present_pending && h.fake.registrations == 0 && h.fake.signals == 0);
    CHECK(cancel(h, token) && idle(h, 0, sparse) && no_refs(h));
    CHECK(WaitForSingleObject(h.worker->output_event, 2000) == WAIT_OBJECT_0);
    CHECK(h.worker->poster_present_mask == 0);
    const auto old = token;
    CHECK(seed(h, 0, 2, &token, &sparse));
    CHECK(account_media_worker_local_preview_uploaded(h.worker, old).state == AccountMediaRequestState::invalid);
    CHECK(uploaded(h, token) && arm(h));
    CHECK(phase(lane) == AccountMediaWorkerPhase::poster_fence_wait && lane.poster_registered && h.fake.event == lane.command_event);
    CHECK(account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::ready);
    CHECK(cancel(h, token));
    CHECK(phase(lane) == AccountMediaWorkerPhase::poster_fence_wait && lane.media.progressive && lane.media.owned_mp4_decoder.ready);
    CHECK(WaitForSingleObject(lane.command_event, 0) == WAIT_TIMEOUT);
    ResetEvent(h.worker->output_event);
    CHECK(SetEvent(h.fake.event) && idle(h, 0, sparse) && no_refs(h)); h.fake.event = nullptr;
    CHECK(WaitForSingleObject(h.worker->output_event, 0) == WAIT_TIMEOUT);
    CHECK(h.fake.last_release_thread == lane.thread_id && h.fake.last_release_thread != GetCurrentThreadId());
    CHECK(account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::ready);
    return true;
}
bool failure_cases(Harness& h) noexcept {
    const Failure failures[]{Failure::context, Failure::device, Failure::create, Failure::signal, Failure::registration};
    u64 serial = 10;
    for (const auto failure : failures) {
        AccountMediaLocalPreviewRequest token{}; const u8* sparse{};
        h.worker->poster_disabled_device = nullptr; h.fake.failure = failure;
        CHECK(seed(h, 0, serial++, &token, &sparse) && uploaded(h, token) && arm(h));
        auto& lane = h.worker->lanes[0];
        CHECK(phase(lane) == AccountMediaWorkerPhase::leased && !lane.poster_registered && lane.media.progressive);
        CHECK(h.worker->poster_disabled_device == lane.command.video_device);
        const LONG registrations = h.fake.registrations, signals = h.fake.signals;
        CHECK(account_media_worker_local_preview_uploaded(h.worker, token).state == AccountMediaRequestState::ready && arm(h));
        CHECK(h.fake.registrations == registrations && h.fake.signals == signals);
        CHECK(cancel(h, token) && idle(h, 0, sparse) && no_refs(h));
    }
    h.fake.failure = Failure::none; h.worker->poster_disabled_device = nullptr;
    return true;
}
bool pressure_case(Harness& h) noexcept {
    AccountMediaLocalPreviewRequest tokens[kAccountMediaDecodeLaneCount]{};
    const u8* sparse[kAccountMediaDecodeLaneCount]{};
    for (u32 i = 0; i < kAccountMediaDecodeLaneCount; ++i)
        CHECK(seed(h, i, 100 + i, &tokens[i], &sparse[i]) && uploaded(h, tokens[i]));
    AccountMediaLocalPreviewRequest request{};
    request.content = {&h, &source_ops, 1}; request.snapshot = {{600, 19, 3, 0}, 1};
    request.key = media_asset_local_draft_preview(600, 1, MediaAttachmentFormat::png);
    request.owner_scope = 700; request.owner_generation = 8;
    ResetEvent(h.worker->output_event);
    CHECK(account_media_worker_request_local_preview(h.worker, request, 1, 1).state == AccountMediaRequestState::busy);
    CHECK(ReadAcquire(&h.worker->poster_admission_waiter) != 0);
    CHECK(cancel(h, tokens[0]) && idle(h, 0, sparse[0]));
    CHECK(WaitForSingleObject(h.worker->output_event, 2000) == WAIT_OBJECT_0);
    for (u32 i = 1; i < kAccountMediaDecodeLaneCount; ++i) CHECK(cancel(h, tokens[i]) && idle(h, i, sparse[i]));
    CHECK(no_refs(h)); return true;
}
bool immediate_case(Harness& h) noexcept {
    AccountMediaLocalPreviewRequest token{}; const u8* sparse{};
    h.fake.immediate = true;
    CHECK(seed(h, 0, 200, &token, &sparse) && uploaded(h, token) && arm(h));
    CHECK(idle(h, 0, sparse) && no_refs(h));
    h.fake.event = nullptr; h.fake.immediate = false;
    return true;
}
bool timeout_case(Harness& h) noexcept {
    AccountMediaLocalPreviewRequest token{}; const u8* sparse{};
    CHECK(seed(h, 0, 300, &token, &sparse) && uploaded(h, token) && arm(h));
    const HANDLE event = h.fake.event; DWORD flags{};
    CHECK(!account_media_worker_stop(h.worker, 5));
    CHECK(h.worker->initialized && GetHandleInformation(event, &flags));
    CHECK(h.worker->lanes[0].poster_registered && h.worker->lanes[0].media.progressive);
    CHECK(h.fake.fence.references == 2 && h.fake.context.references == 2);
    CHECK(SetEvent(event) && account_media_worker_stop(h.worker, 2000)); h.fake.event = nullptr;
    CHECK(!h.worker->initialized && no_refs(h));
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(sparse, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE);
    return true;
}
bool simultaneous_stop_case(Harness& h) noexcept {
    CHECK(begin(h));
    AccountMediaLocalPreviewRequest token{}; const u8* sparse{};
    CHECK(seed(h, 0, 400, &token, &sparse) && uploaded(h, token) && arm(h));
    auto& lane = h.worker->lanes[0];
    AcquireSRWLockExclusive(lane.poster_lock.native());
    const bool signaled = SetEvent(h.worker->stop_event) && SetEvent(h.fake.event);
    ReleaseSRWLockExclusive(lane.poster_lock.native());
    CHECK(signaled && account_media_worker_stop(h.worker, 2000)); h.fake.event = nullptr;
    CHECK(no_refs(h));
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(sparse, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE);
    return true;
}
bool run() noexcept {
    deadline = GetTickCount64() + 8'000;
    Harness h;
    CHECK(begin(h) && ordinary_cases(h) && failure_cases(h) && pressure_case(h) && immediate_case(h));
    CHECK(timeout_case(h) && simultaneous_stop_case(h));
    return true;
}
DWORD WINAPI watchdog(void* stopped) noexcept {
    if (WaitForSingleObject(static_cast<HANDLE>(stopped), 9'000) != WAIT_OBJECT_0) ExitProcess(2);
    return 0;
}
}}
extern "C" void maicord_media_worker_poster_lease_entry() {
    HANDLE stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watcher = stopped ? CreateThread(nullptr, 0, maicord::watchdog, stopped, 0, nullptr) : nullptr;
    if (!watcher) ExitProcess(2);
    const bool ok = maicord::run(); SetEvent(stopped);
    const bool joined = WaitForSingleObject(watcher, 100) == WAIT_OBJECT_0;
    CloseHandle(watcher); CloseHandle(stopped); ExitProcess(ok && joined ? 0 : 1);
}

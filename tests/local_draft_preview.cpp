#include "maicord/app/account_media_worker.hpp"
#include "maicord/core/time.hpp"
#include "png_growth_fixture.hpp"
#include "jpeg_reference_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace local_preview_test {
struct Source {
    ConstSlice<u8> bytes;
    HANDLE entered, allowed;
    DWORD ui_thread;
    volatile LONG references, retains, releases, reads, ui_reads;
    bool held;
};
void retain(void* value) noexcept {
    auto& source = *static_cast<Source*>(value);
    InterlockedIncrement(&source.references);
    InterlockedIncrement(&source.retains);
}
void release(void* value) noexcept {
    auto& source = *static_cast<Source*>(value);
    InterlockedDecrement(&source.references);
    InterlockedIncrement(&source.releases);
}
ConstSlice<u8> read(void* value, u64 offset, Slice<u8> scratch) noexcept {
    // This fixture exercises the ordinary short-read and cancellation path.
    if (!scratch.data || !scratch.count)
        return {};
    auto& source = *static_cast<Source*>(value);
    InterlockedIncrement(&source.reads);
    if (GetCurrentThreadId() == source.ui_thread)
        InterlockedIncrement(&source.ui_reads);
    SetEvent(source.entered);
    if (source.held && WaitForSingleObject(source.allowed, 5000) != WAIT_OBJECT_0)
        return {};
    if (offset >= source.bytes.count)
        return {};
    // A deliberately short borrowed read exercises the actual source contract.
    return {source.bytes.data + offset,
            minimum<u32>(minimum<u32>(scratch.count, 23), source.bytes.count - static_cast<u32>(offset))};
}
const UploadSourceOps operations{retain, release, read};
bool begin(Source& source, ConstSlice<u8> bytes, bool held) noexcept {
    source.bytes = bytes;
    source.held = held;
    source.references = 1;
    source.ui_thread = GetCurrentThreadId();
    source.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    source.allowed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return source.entered && source.allowed;
}
AccountMediaLocalPreviewRequest request(Source& source, u64 id, u32 generation,
                                        MediaAttachmentFormat format) noexcept {
    return {{&source, &operations, source.bytes.count},
            {{id, 19, 3, 0}, source.bytes.count},
            {},
            media_asset_local_draft_preview(id, generation, format),
            700,
            8,
            0};
}
AccountMediaLocalPreviewRequest retirement(const AccountMediaLocalPreviewRequest& source) noexcept {
    AccountMediaLocalPreviewRequest exact{};
    exact.key = source.key;
    exact.owner_scope = source.owner_scope;
    exact.owner_generation = source.owner_generation;
    return exact;
}
AccountMediaRequestResult completion(AccountMediaWorker* worker,
                                     const AccountMediaLocalPreviewRequest& request) noexcept {
    AccountMediaRequestResult result{};
    const u64 until = GetTickCount64() + 5000;
    do {
        result =
            account_media_worker_request_local_preview(worker, request, clock_monotonic_milliseconds(), 1);
        if (result.state != AccountMediaRequestState::pending &&
            result.state != AccountMediaRequestState::queued &&
            result.state != AccountMediaRequestState::busy)
            return result;
        WaitForSingleObject(account_media_worker_notification_handle(worker), 20);
    } while (GetTickCount64() < until);
    return result;
}
void close(Source& source) noexcept {
    if (source.entered)
        CloseHandle(source.entered);
    if (source.allowed)
        CloseHandle(source.allowed);
}
} // namespace local_preview_test
void test_local_draft_preview(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace local_preview_test;
    Source png{}, jpeg{}, held{};
    bool sources = begin(png, {kPngGrowthFixture, sizeof(kPngGrowthFixture)}, false) &&
                   begin(jpeg, {kJpeg420Reference, sizeof(kJpeg420Reference)}, false) &&
                   begin(held, {kPngGrowthFixture, sizeof(kPngGrowthFixture)}, true);
    auto first = request(png, 900, 1, MediaAttachmentFormat::png);
    check(sources && local_draft_preview_request_valid(first) && media_asset_large_preview(first.key) &&
              first.key.kind != MediaAssetKind::attachment,
          "local preview: separate exact draft identity preserves frame ABI and bounded typed source");
    auto bad = first;
    bad.snapshot.bytes += 1;
    check(!local_draft_preview_request_valid(bad),
          "local preview: mismatched retained source size rejects before admission");
    bad = first;
    bad.snapshot.identity.reserved = 1;
    check(!local_draft_preview_request_valid(bad),
          "local preview: noncanonical snapshot rejects before admission");
    bad = first;
    bad.snapshot.bytes = 16u * 1024u * 1024u + 1;
    bad.content.bytes = bad.snapshot.bytes;
    check(!local_draft_preview_request_valid(bad) &&
              !media_asset_key_valid(media_asset_local_draft_preview(900, 2, MediaAttachmentFormat::mp4)),
          "local preview: encoded bound and unsupported video remain placeholders");
    auto* worker = static_cast<AccountMediaWorker*>(
        VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const bool initialized = sources && worker && account_media_worker_initialize(worker).ok();
    check(initialized,
          "local preview: existing eight decode lanes initialize without extra thread or network request");
    if (!initialized) {
        if (worker)
            VirtualFree(worker, 0, MEM_RELEASE);
        close(png);
        close(jpeg);
        close(held);
        return;
    }
    auto blocked = request(held, 901, 2, MediaAttachmentFormat::png);
    const auto queued =
        account_media_worker_request_local_preview(worker, blocked, clock_monotonic_milliseconds(), 1);
    const bool entered = WaitForSingleObject(held.entered, 5000) == WAIT_OBJECT_0;
    check(queued.state == AccountMediaRequestState::queued && entered && held.references == 2 &&
              held.ui_reads == 0,
          "local preview: actual retained source read runs on decode worker while admission stays "
          "nonblocking");
    bad = retirement(blocked);
    ++bad.owner_scope;
    check(account_media_worker_retire_local_preview(worker, bad).state == AccountMediaRequestState::ready &&
              held.references == 2,
          "local preview: different owner cannot cancel the exact retained decode");
    bad = blocked;
    ++bad.snapshot.identity.last_write;
    check(
        account_media_worker_request_local_preview(worker, bad, clock_monotonic_milliseconds(), 1).state ==
            AccountMediaRequestState::invalid,
        "local preview: a live atlas key cannot be retargeted to another source snapshot");
    const auto cancelled = account_media_worker_retire_local_preview(worker, retirement(blocked));
    check(cancelled.state == AccountMediaRequestState::pending && held.references == 2 &&
              held.releases == 0,
          "local preview: cancellation retains source until the worker acknowledges its borrowed read");
    SetEvent(held.allowed);
    const u64 cancelled_until = GetTickCount64() + 5000;
    while (held.references != 1 && GetTickCount64() < cancelled_until)
        WaitForSingleObject(account_media_worker_notification_handle(worker), 20);
    AccountMediaFrame frame{};
    check(held.references == 1 && held.retains == 1 && held.releases == 1 &&
              !account_media_worker_take_frame(worker, &frame),
          "local preview: exact cancelled output is drained without stale frame publication or early "
          "source release");
    check(completion(worker, first).state == AccountMediaRequestState::ready &&
              account_media_worker_take_frame(worker, &frame) &&
              frame.kind == AccountMediaFrameKind::png_scanlines && frame.width == 1536 &&
              frame.height == 1536 && frame.frame_count == 1 && frame.duration_us == 0 &&
              local_draft_preview_frame_matches(frame, first) && png.references == 1,
          "local preview: shipping PNG codec publishes bounded static first frame with exact owner and "
          "generation");
    bad = first;
    ++bad.owner_generation;
    check(!local_draft_preview_frame_matches(frame, bad),
          "local preview: retired owner publication cannot accept a decoded frame");
    bad = first;
    ++bad.key.variant;
    check(!local_draft_preview_frame_matches(frame, bad),
          "local preview: reused row generation cannot accept a decoded frame");
    const LONG prior_reads = png.reads;
    check(completion(worker, first).state == AccountMediaRequestState::ready &&
              account_media_worker_take_frame(worker, &frame) && png.reads == prior_reads &&
              png.references == 1,
          "local preview: atlas residency recovery reuses encoded cache without another source read");
    auto jpeg_request = request(jpeg, 902, 3, MediaAttachmentFormat::jpeg);
    check(completion(worker, jpeg_request).state == AccountMediaRequestState::ready &&
              account_media_worker_take_frame(worker, &frame) &&
              frame.kind == AccountMediaFrameKind::jpeg_coefficients && frame.width == 16 &&
              frame.height == 16 && local_draft_preview_frame_matches(frame, jpeg_request) &&
              jpeg.references == 1 && jpeg.ui_reads == 0,
          "local preview: shipping JPEG workspace carries static provenance without CDN or UI decode");
    wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{}, borrowed_path[MAX_PATH]{};
    DWORD written = 0;
    FileSnapshot file_snapshot{};
    bool file_fixture = GetTempPathW(MAX_PATH, folder) && GetTempFileNameW(folder, L"MLP", 0, path);
    HANDLE file = file_fixture ? CreateFileW(path, GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL, nullptr)
                               : INVALID_HANDLE_VALUE;
    file_fixture = file != INVALID_HANDLE_VALUE &&
                   WriteFile(file, kJpeg420Reference, sizeof(kJpeg420Reference), &written, nullptr) &&
                   written == sizeof(kJpeg420Reference);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    // Windows finalizes last-write metadata when the writer closes.
    file = file_fixture ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr)
                        : INVALID_HANDLE_VALUE;
    file_fixture = file != INVALID_HANDLE_VALUE && file_snapshot_read(file, &file_snapshot);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    u32 units = 0;
    while (path[units] && units + 1 < MAX_PATH) {
        borrowed_path[units] = path[units];
        ++units;
    }
    AccountMediaLocalPreviewRequest file_request{
        {},
        file_snapshot,
        {borrowed_path, units},
        media_asset_local_draft_preview(903, 4, MediaAttachmentFormat::jpeg),
        700,
        8,
        0};
    const auto file_queued = file_fixture ? account_media_worker_request_local_preview(
                                                worker, file_request, clock_monotonic_milliseconds(), 1)
                                          : AccountMediaRequestResult{};
    // Invalidate the caller's path immediately after admission. The lane's
    // retained command is the sole path used by the shipping file reader.
    borrowed_path[0] = L'X';
    AccountMediaLocalPreviewRequest file_poll = file_request;
    file_poll.path = {path, units};
    check(
        file_fixture && file_queued.state == AccountMediaRequestState::queued &&
            completion(worker, file_poll).state == AccountMediaRequestState::ready &&
            account_media_worker_take_frame(worker, &frame) &&
            local_draft_preview_frame_matches(frame, file_poll),
        "local preview: path is copied before return and worker validates actual file identity and bytes");
    auto replaced = file_poll;
    replaced.key = media_asset_local_draft_preview(904, 5, MediaAttachmentFormat::jpeg);
    ++replaced.snapshot.identity.last_write;
    check(file_fixture && completion(worker, replaced).state == AccountMediaRequestState::failed &&
              !account_media_worker_take_frame(worker, &frame),
          "local preview: mismatched filesystem snapshot produces no decoded receipt");
    if (path[0])
        DeleteFileW(path);
    bool no_network = true;
    for (const auto& lane : worker->lanes)
        no_network = no_network && !lane.media.client_started && !lane.local_retained;
    check(no_network && png.ui_reads == 0 && png.retains == png.releases && jpeg.retains == jpeg.releases,
          "local preview: all optional decodes balance source ownership and leave network clients stopped");
    const bool stopped = account_media_worker_stop(worker, 5000);
    check(stopped,
          "local preview: existing account stop joins decode owners before releasing shared cache");
    if (stopped) {
        VirtualFree(worker, 0, MEM_RELEASE);
        close(png);
        close(jpeg);
        close(held);
    }
}
} // namespace maicord

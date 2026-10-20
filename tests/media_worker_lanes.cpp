// Media decode lanes through the production worker: initialization starts no
// lane thread, lanes start (storage and thread) on their first command, and
// the deepest decode the lanes run (PNG/APNG, JPEG, lossless/lossy/animated
// WebP, GIF and the largest bundled regional flag) keeps half the lane stack
// reservation free.
#include "maicord/app/account_media_worker.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/asset.hpp"
#include "fixtures/image_decode_corpus.hpp"
#include "media_fixture.hpp"
#include "png_growth_fixture.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_exception.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

// Reservation and committed bytes of a thread's stack, from its TEB.
struct StackUse {
    u64 reserved, committed;
};
using QueryThread = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
StackUse stack_use(HANDLE thread) noexcept {
    static const auto query = reinterpret_cast<QueryThread>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    struct {
        LONG exit_status;
        void* teb;
        void* client[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } basic{};
    if (!thread || !query || query(thread, 0, &basic, sizeof(basic), nullptr) < 0 || !basic.teb)
        return {};
    const u64 base = reinterpret_cast<u64>(static_cast<const NT_TIB*>(basic.teb)->StackBase);
    const u64 deallocation = *reinterpret_cast<const u64*>(static_cast<const u8*>(basic.teb) + 0x1478);
    u64 committed = 0;
    for (u64 at = deallocation; at < base;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || !info.RegionSize)
            break;
        const u64 end = minimum<u64>(base, reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize);
        if (info.State == MEM_COMMIT)
            committed += end - at;
        at = end;
    }
    return {base - deallocation, committed};
}

struct Source {
    ConstSlice<u8> bytes;
    volatile LONG references;
};
void retain(void* value) noexcept {
    InterlockedIncrement(&static_cast<Source*>(value)->references);
}
void release(void* value) noexcept {
    InterlockedDecrement(&static_cast<Source*>(value)->references);
}
ConstSlice<u8> read(void* value, u64 offset, Slice<u8> scratch) noexcept {
    const auto& source = *static_cast<Source*>(value);
    if (!scratch.data || offset >= source.bytes.count)
        return {};
    return {source.bytes.data + offset,
            minimum<u32>(scratch.count, source.bytes.count - static_cast<u32>(offset))};
}
const UploadSourceOps operations{retain, release, read};

AccountMediaLocalPreviewRequest local_request(Source& source, u64 id,
                                              MediaAttachmentFormat format) noexcept {
    return {{&source, &operations, source.bytes.count},
            {{id, 19, 3, 0}, source.bytes.count},
            {},
            media_asset_local_draft_preview(id, 1, format),
            700,
            8,
            0};
}

// Polls one request to its final state and leases the frame it published.
template <typename Request>
AccountMediaRequestState complete(AccountMediaWorker* worker, Request request) noexcept {
    AccountMediaRequestResult result{};
    const u64 until = GetTickCount64() + 5'000;
    do {
        result = request();
        if (result.state != AccountMediaRequestState::pending &&
            result.state != AccountMediaRequestState::queued &&
            result.state != AccountMediaRequestState::busy)
            break;
        WaitForSingleObject(account_media_worker_notification_handle(worker), 20);
    } while (GetTickCount64() < until);
    AccountMediaFrame frame{};
    if (result.state == AccountMediaRequestState::ready)
        account_media_worker_take_frame(worker, &frame);
    return result.state;
}

MediaAttachmentFormat corpus_format(ImageCorpusKind kind) noexcept {
    return kind == ImageCorpusKind::jpeg                                   ? MediaAttachmentFormat::jpeg
           : kind == ImageCorpusKind::png || kind == ImageCorpusKind::apng ? MediaAttachmentFormat::png
                                                                           : MediaAttachmentFormat::webp;
}

constexpr u32 kCorpusCount = sizeof(kImageCorpus) / sizeof(kImageCorpus[0]);
u8 gif_encoded[256 * 1024], gif_scratch[256 * 1024];

// Two animations whose uncached frames alternate, through idle lanes: counts
// frames decoded on a lane without the source's retained container parse.
u32 alternating_parses(bool affinity) noexcept {
    auto* worker = static_cast<AccountMediaWorker*>(
        VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!worker)
        return ~0u;
    for (auto& lane : worker->lanes)
        lane.phase = static_cast<LONG>(AccountMediaWorkerPhase::idle);
    const MediaAssetKey keys[2]{media_asset_sticker(71, 2), media_asset_sticker(72, 2)};
    u32 parses = 0;
    for (u32 step = 0; step < 12; ++step) {
        const MediaAssetKey key = keys[step & 1];
        const bool advance = step >= 2; // first frames arrive as asset requests
        u32 index = advance && affinity ? account_media_worker_animation_lane(worker, key)
                                        : kAccountMediaDecodeLaneCount;
        if (index == kAccountMediaDecodeLaneCount)
            index = 0; // first idle lane: animated work starts at lane 0
        auto& lane = worker->lanes[index];
        parses += !(lane.initialized && media_asset_key_equal(lane.command.key, key));
        lane.initialized = true;
        lane.command.key = key;
        lane.command.kind =
            advance ? AccountMediaWorkKind::animation_advance : AccountMediaWorkKind::static_asset;
    }
    VirtualFree(worker, 0, MEM_RELEASE);
    return parses;
}

void run() noexcept {
    auto* worker = static_cast<AccountMediaWorker*>(
        VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(worker != nullptr, "media lanes: worker storage");
    if (!worker)
        return;
    const bool initialized = account_media_worker_initialize(worker).ok();
    check(initialized, "media lanes: worker initializes");
    if (!initialized)
        return;
    bool none_started = true;
    for (const auto& lane : worker->lanes)
        none_started = none_started && !lane.thread && lane.initialized == (lane.index == 0) &&
                       lane.media.started == (lane.index == 0);
    check(none_started &&
              account_media_worker_start_lane(worker, kAccountMediaDecodeLaneCount).state ==
                  AccountMediaRequestState::invalid,
          "media lanes: initialization starts no decode thread and only the cache owner's storage");

    // No live source or network: admission must keep a collection switch off
    // spare lanes while an earlier track still owns this embed's lane.
    {
        const auto key = media_asset_embed(700, 1, 0, MediaEmbedResource::video, MediaAttachmentFormat::m4a);
        AccountMediaAttachmentRequest request{utf8_literal("https://soundcloud.com/forss/sets/soulhack"), key,
            700, 1, 0, 0, CdnImageType::m4a, {1, {0, 0}, 2}, nullptr};
        auto& lane = worker->lanes[1];
        lane.command.key = key; lane.command.kind = AccountMediaWorkKind::attachment;
        lane.command.source_message_id = 700; lane.command.source_message_revision = 1;
        lane.command.collection = 1; lane.command.collection_track = 1;
        const AccountMediaWorkerPhase phases[]{AccountMediaWorkerPhase::selected_video,
            AccountMediaWorkerPhase::working, AccountMediaWorkerPhase::command_ready};
        for (auto phase : phases) {
            lane.phase = static_cast<LONG>(phase);
            check(account_media_worker_request_attachment(worker, request, 1, 1).state == AccountMediaRequestState::busy &&
                !ReadAcquire64(&worker->counters.queued), "collection switching waits for the old track owner without queuing on a spare lane");
        }
        request.options.collection_track = 1;
        check(account_media_worker_request_attachment(worker, request, 1, 1).state == AccountMediaRequestState::pending,
            "same collection track request reuses the outstanding command");
        request.options.collection_track = 8193;
        check(account_media_worker_request_attachment(worker, request, 1, 1).state == AccountMediaRequestState::invalid,
            "collection request rejects an index beyond the bounded queue before any work");
        request.options.collection_track = 1; request.signed_url = utf8_literal("https://soundcloud.com/forss/flickermood");
        check(account_media_worker_request_attachment(worker, request, 1, 1).state == AccountMediaRequestState::invalid,
            "explicit collection index cannot alter an ordinary single-track request");
        lane.command = {}; lane.phase = static_cast<LONG>(AccountMediaWorkerPhase::idle);
    }

    // Every decoder kind once, one request at a time. Animated corpus entries
    // reach their parsers too; local previews admit only still first frames.
    static Source sources[kCorpusCount + 2]{};
    u32 stills = 0, stills_ready = 0;
    u64 id = 1'000;
    for (u32 i = 0; i < kCorpusCount; ++i) {
        auto& source = sources[i];
        source.bytes = {kImageCorpus[i].data, kImageCorpus[i].bytes};
        source.references = 1;
        const auto request = local_request(source, ++id, corpus_format(kImageCorpus[i].kind));
        const bool still = kImageCorpus[i].kind != ImageCorpusKind::apng &&
                           kImageCorpus[i].kind != ImageCorpusKind::webp_animated;
        const bool ready = complete(worker, [&]() noexcept {
                               return account_media_worker_request_local_preview(
                                   worker, request, clock_monotonic_milliseconds(), 1);
                           }) == AccountMediaRequestState::ready;
        stills += still;
        stills_ready += still && ready;
    }
    GifFixtureResult gif{};
    const bool gif_built = build_indexed_gif_fixture({gif_encoded, sizeof(gif_encoded)},
                                                     {gif_scratch, sizeof(gif_scratch)}, &gif);
    auto& gif_source = sources[kCorpusCount];
    gif_source.bytes = {gif_encoded, gif.encoded_bytes};
    gif_source.references = 1;
    const auto gif_request = local_request(gif_source, ++id, MediaAttachmentFormat::gif);
    const bool gif_ready = gif_built && complete(worker, [&]() noexcept {
                                            return account_media_worker_request_local_preview(
                                                worker, gif_request, clock_monotonic_milliseconds(), 1);
                                        }) == AccountMediaRequestState::ready;
    auto& large = sources[kCorpusCount + 1];
    large.bytes = {kPngGrowthFixture, sizeof(kPngGrowthFixture)};
    large.references = 1;
    const auto large_request = local_request(large, ++id, MediaAttachmentFormat::png);
    const bool large_ready = complete(worker, [&]() noexcept {
                                 return account_media_worker_request_local_preview(
                                     worker, large_request, clock_monotonic_milliseconds(), 1);
                             }) == AccountMediaRequestState::ready;
    // Sequence 69 is the bundle's largest flag (its group inflates 52,105
    // bytes): inflate scratch, SVG parse and paint compile, the deepest decode.
    const bool flag_ready =
        complete(worker, [&]() noexcept {
            return account_media_worker_request_static_asset(worker, media_asset_regional_flag(69),
                                                             clock_monotonic_milliseconds(), 1);
        }) == AccountMediaRequestState::ready;
    check(stills && stills_ready == stills && gif_ready && large_ready && flag_ready,
          "media lanes: every still corpus image, GIF, 1536 px PNG and bundled flag decodes on a lane");
    const auto started_lanes = [&]() noexcept {
        u32 started = 0;
        bool consistent = true;
        for (const auto& lane : worker->lanes) {
            started += lane.thread != nullptr;
            consistent = consistent &&
                         (!lane.thread || (lane.initialized && lane.media.started &&
                                           lane.media.pipeline.cache == &worker->lanes[0].media.cache));
        }
        return consistent ? started : ~0u;
    };
    const u32 sequential_lanes = started_lanes();
    check(sequential_lanes >= 1 && sequential_lanes <= 2,
          "media lanes: one request at a time starts at most the animation and still-image lanes");

    // A burst wider than the lanes: sixteen distinct keys at once.
    static Source burst[16]{};
    AccountMediaLocalPreviewRequest burst_requests[16]{};
    for (u32 i = 0; i < 16; ++i) {
        burst[i].bytes = {kPngGrowthFixture, sizeof(kPngGrowthFixture)};
        burst[i].references = 1;
        burst_requests[i] = local_request(burst[i], 5'000 + i, MediaAttachmentFormat::png);
        account_media_worker_request_local_preview(worker, burst_requests[i],
                                                   clock_monotonic_milliseconds(), 1);
    }
    u32 burst_ready = 0;
    for (u32 i = 0; i < 16; ++i)
        burst_ready += complete(worker, [&]() noexcept {
                           return account_media_worker_request_local_preview(
                               worker, burst_requests[i], clock_monotonic_milliseconds(), 1);
                       }) == AccountMediaRequestState::ready;
    const u32 burst_lanes = started_lanes();
    check(burst_ready == 16 && burst_lanes > sequential_lanes &&
              burst_lanes <= kAccountMediaDecodeLaneCount,
          "media lanes: a burst starts further lanes on demand, one thread per started lane");
    // Measured: the bundled flag commits 152 KiB of lane stack, a 1536 px PNG 40 KiB.
    bool margin = true;
    for (const auto& lane : worker->lanes) {
        const auto use = stack_use(lane.thread);
        margin = margin && (!lane.thread || (use.reserved == kAccountMediaWorkerStackBytes &&
                                             use.committed <= kAccountMediaWorkerStackBytes / 2));
    }
    check(margin, "media lanes: the deepest decode commits at most half of the lane stack reservation");
    // 2 first frames + 10 alternating advances: first-idle order re-parses all
    // 12; with affinity only the first frames and one move to a spare lane.
    check(alternating_parses(false) == 12 && alternating_parses(true) == 3,
          "media lanes: alternating animation frames keep to the lane holding their parse");
    const bool stopped = account_media_worker_stop(worker, 5'000);
    check(stopped, "media lanes: stop joins every lane");
    if (stopped)
        VirtualFree(worker, 0, MEM_RELEASE);
}
} // namespace
} // namespace maicord

extern "C" void maicord_media_worker_lanes_entry() {
    maicord::run();
    ExitProcess(maicord::failures ? 1 : 0);
}

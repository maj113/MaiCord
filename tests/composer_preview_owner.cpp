#include "../src/app/application_attachments_state.hpp"
#include "../src/app/application_composer_preview.hpp"
#include "../src/app/application_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

// Shipping composer preview owner with a controlled account boundary: MP4/MOV
// drafts request a GPU poster (with the renderer's device), unsupported video
// keeps the file icon, and removing or sending a draft retires the exact
// preview so the lane releases its decoder and the atlas entry is evictable.
namespace maicord {
namespace composer_preview_owner_test {
using namespace app_detail;
u8 actor{}, device_token{};
u32 requests{}, retires{};
AccountMediaLocalPreviewRequest submitted{}, retired{};
bool request_shape_ok = true;

AccountMediaRequestResult WINAPI media(void* instance, AccountLocalPreviewOperation operation,
                                       const AccountMediaLocalPreviewRequest* request, u64, u64) noexcept {
    AccountMediaRequestResult result{};
    if (instance != &actor) {
        result.state = AccountMediaRequestState::invalid;
        return result;
    }
    if (operation == AccountLocalPreviewOperation::presented) {
        result.state = request ? AccountMediaRequestState::invalid : AccountMediaRequestState::ready;
        return result;
    }
    if (!request) { result.state = AccountMediaRequestState::invalid; return result; }
    if (operation == AccountLocalPreviewOperation::uploaded) {
        result.state = request->lease_serial ? AccountMediaRequestState::ready : AccountMediaRequestState::invalid;
        return result;
    }
    if (operation == AccountLocalPreviewOperation::retire) {
        ++retires;
        retired = *request;
        result.state = AccountMediaRequestState::ready;
        return result;
    }
    if (operation != AccountLocalPreviewOperation::request) return result;
    ++requests;
    submitted = *request;
    request_shape_ok = request_shape_ok && local_draft_preview_request_valid(*request);
    result.state = AccountMediaRequestState::queued;
    return result;
}
AccountMediaFrame receipt() noexcept {
    AccountMediaFrame frame{};
    frame.key = submitted.key;
    frame.source_kind = AccountMediaSourceKind::local_draft_preview;
    frame.source_catalog_scope = submitted.owner_scope;
    frame.source_catalog_generation = submitted.owner_generation;
    frame.source_message_id = submitted.key.id;
    frame.source_message_revision = submitted.key.variant;
    frame.kind = AccountMediaFrameKind::video_nv12_gpu;
    frame.width = 384;
    frame.height = 216;
    return frame;
}
} // namespace composer_preview_owner_test

void test_composer_preview_owner(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    using namespace composer_preview_owner_test;
    auto* const saved_ui = g_app.account_ui;
    auto* const saved_attachments = g_app.attachments;
    void* const saved_instance = g_app.account.instance;
    const u64 saved_profile = g_app.account.status.profile_id;
    const auto saved_media = g_app.account.api.media_local_preview;
    auto* const saved_atlas = g_app.renderer.preview_media_atlas;
    auto* const saved_device = g_app.renderer.graphics.device;
    static MediaAtlas atlas;
    static MediaAtlasEntry entries[16];
    static u32 buckets[32];
    static GlyphAtlasEntry allocations[16];
    static GlyphAtlasRow rows[16];
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.attachments = static_cast<ApplicationAttachments*>(
        VirtualAlloc(nullptr, sizeof(ApplicationAttachments), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* state = g_app.attachments;
    const bool ready = g_app.account_ui && state && message_attachment_drafts_initialize(&state->drafts) &&
                       media_atlas_initialize(&atlas, 1024, 1024, 1, {entries, 16}, {buckets, 32},
                                              {allocations, 16}, {rows, 16});
    check(ready, "composer preview owner: fixture allocation");
    if (ready) {
        g_app.account.instance = &actor;
        g_app.account.status.profile_id = 44;
        g_app.account.api.media_local_preview = media;
        g_app.account_ui->active_channel_id = 7;
        g_app.renderer.preview_media_atlas = &atlas;
        g_app.renderer.graphics.device = reinterpret_cast<ID3D11Device*>(&device_token);
        state->profile_id = 44;
        const wchar_t movie_path[] = L"C:\\clip.MOV", webm_path[] = L"C:\\clip.webm";
        const MessageAttachmentFile movie{{movie_path, 11}, {{21, 22, 23, 0}, 200ull * 1024u * 1024u}};
        const MessageAttachmentFile webm{{webm_path, 12}, {{24, 25, 26, 0}, 4096}};
        u64 movie_id = 0, webm_id = 0;
        message_attachment_drafts_add(&state->drafts, 7, {&movie, 1}, {&movie_id, 1});
        message_attachment_drafts_add(&state->drafts, 7, {&webm, 1}, {&webm_id, 1});
        application_composer_preview_request(1000, 5);
        check(requests == 1 && request_shape_ok && media_asset_catalog_format(submitted.key) == MediaAttachmentFormat::mp4 &&
                  local_draft_preview_thumbnail(submitted.key) &&
                  submitted.video_device == reinterpret_cast<ID3D11Device*>(&device_token) &&
                  submitted.path.count == 11 && submitted.snapshot.bytes == 200ull * 1024u * 1024u,
              "MOV drafts request a GPU poster with the renderer device and the 200 MB source unread");
        check(application_composer_preview_view(webm_id).failed && !application_composer_preview_view(movie_id).ready,
              "WebM drafts keep the video file icon without a decode request");
        auto frame = receipt();
        const bool uploaded = media_atlas_allocate(&atlas, frame.key, 384, 216).ok();
        application_composer_preview_frame_uploaded(frame);
        check(uploaded && application_composer_preview_view(movie_id).ready &&
                  application_composer_preview_metrics().ready == 1,
              "an uploaded poster frame makes the tile's thumbnail ready");
        message_attachment_drafts_remove(&state->drafts, movie_id);
        application_composer_preview_request(1100, 5);
        check(retires == 1 && media_asset_key_equal(retired.key, frame.key) &&
                  retired.owner_generation == frame.source_catalog_generation &&
                  !application_composer_preview_key_current(frame.key) &&
                  application_composer_preview_metrics().ready == 0,
              "removing a video draft retires its exact poster so the lane and atlas release it");
        u64 sent_id = 0;
        message_attachment_drafts_add(&state->drafts, 7, {&movie, 1}, {&sent_id, 1});
        application_composer_preview_request(1200, 5);
        const auto sent_key = submitted.key;
        if (auto* record = message_attachment_drafts_get(&state->drafts, sent_id))
            record->phase = MessageAttachmentDraftPhase::queued;
        application_composer_preview_request(1300, 5);
        check(requests == 2 && retires == 2 && media_asset_key_equal(retired.key, sent_key) &&
                  !media_asset_key_equal(sent_key, frame.key),
              "sending a video draft retires its poster under a fresh preview generation");
        u64 rejected_id = 0;
        message_attachment_drafts_add(&state->drafts, 7, {&movie, 1}, {&rejected_id, 1});
        application_composer_preview_request(1400, 5);
        application_composer_preview_rejected(submitted.key);
        check(requests == 3 && application_composer_preview_view(rejected_id).failed,
              "an unsupported codec or failed decode falls back to the video file icon");
        // Atlas pressure: a deferred thumbnail waits for the shared retry
        // deadline rather than re-decoding on every 100 ms poll.
        u64 deferred_id = 0;
        message_attachment_drafts_add(&state->drafts, 7, {&movie, 1}, {&deferred_id, 1});
        application_composer_preview_request(1500, 5);
        const u32 deferred_requests = requests;
        application_composer_preview_deferred(submitted.key, 2500);
        application_composer_preview_request(1700, 5);
        const bool waited = requests == deferred_requests;
        application_composer_preview_request(2500, 5);
        check(deferred_requests == 4 && waited && requests == deferred_requests + 1 &&
                  !application_composer_preview_view(deferred_id).failed,
              "a thumbnail deferred for atlas room retries at the shared deadline, not every poll");
        application_composer_preview_release();
        check(application_composer_preview_metrics().active == 0, "composer preview owner releases every entry");
        message_attachment_drafts_release(&state->drafts);
    }
    if (state)
        VirtualFree(state, 0, MEM_RELEASE);
    if (g_app.account_ui)
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = saved_ui;
    g_app.attachments = saved_attachments;
    g_app.account.instance = saved_instance;
    g_app.account.status.profile_id = saved_profile;
    g_app.account.api.media_local_preview = saved_media;
    g_app.renderer.preview_media_atlas = saved_atlas;
    g_app.renderer.graphics.device = saved_device;
}

} // namespace maicord

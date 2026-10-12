// Shipping preview owner with the actual native attachment geometry and atlas.
// Host source authority and the typed DLL callback are controlled seams; codec,
// filesystem, GPU upload and worker retention have separate actual-lane tests.
#include "../src/app/application_forum_preview.hpp"
#include "../src/app/application_forum_attachments.hpp"
#include "../src/app/application_forum_form.hpp"
#include "../src/app/application_state.hpp"
#include "maicord/core/utf.hpp"
#include "../src/app/application_forum_preview.cpp"
#include "test_output.hpp"

namespace maicord::forum_preview_test {
using namespace app_detail;
u32 failures{}, frames{}, requests{}, retires{};
u8 actor{}, replacement{};
bool present = true, valid = true;
ApplicationForumPostTicket ticket{71, 700, 9};
ApplicationForumAttachmentRow row{900,
                                  utf8_literal("draft.png"),
                                  {},
                                  utf8_literal("Uploading"),
                                  64,
                                  0,
                                  MessageAttachmentDraftPhase::uploading,
                                  false,
                                  true,
                                  false};
FileSnapshot snapshot{{101, 202, 3, 0}, 64};
const wchar_t path[] = LR"(C:\preview-fixture\draft.png)";
AccountMediaLocalPreviewRequest submitted{}, cancelled{};
AccountMediaRequestState next_result = AccountMediaRequestState::queued;
ForumPostScene scene{};
ForumPostContent content{};
UiAttachmentDraftView card{};
UiTextMeasureRecord records[128]{};
UiTextMeasureCache measure{};
MediaAtlas atlas{};
MediaAtlasEntry entries[16]{};
u32 buckets[32]{};
GlyphAtlasEntry allocations[16]{};
GlyphAtlasRow rows[16]{};

void check(bool value, const char* text) noexcept {
    test_check(&failures, value, text);
}
bool same(const ApplicationForumPostTicket& a, const ApplicationForumPostTicket& b) noexcept {
    return a.serial == b.serial && a.parent_id == b.parent_id &&
           a.publication_revision == b.publication_revision;
}
bool text_measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* out) noexcept {
    const auto scalars = utf8_validate(request.text);
    if (!out || !scalars.valid || request.constraint.width <= 0)
        return false;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 count = request.no_wrap ? 1 : maximum(1u, (scalars.codepoints + columns - 1) / columns);
    *out = {{request.no_wrap ? scalars.codepoints * 8.f
                             : minimum(scalars.codepoints * 8.f, request.constraint.width),
             count * 20.f},
            16,
            count};
    return true;
}
AccountMediaRequestResult WINAPI media(void* instance, AccountLocalPreviewOperation operation,
                                       const AccountMediaLocalPreviewRequest* request, u64 now,
                                       u64 unix_now) noexcept {
    AccountMediaRequestResult result{};
    if (instance != &actor) {
        result.error = AccountMediaError::invalid_input;
        return result;
    }
    if (operation == AccountLocalPreviewOperation::presented) {
        result.state = request ? AccountMediaRequestState::invalid : AccountMediaRequestState::ready;
        return result;
    }
    if (!request) { result.error = AccountMediaError::invalid_input; return result; }
    if (operation == AccountLocalPreviewOperation::uploaded) {
        result.state = request->lease_serial ? AccountMediaRequestState::ready : AccountMediaRequestState::invalid;
        return result;
    }
    if (operation == AccountLocalPreviewOperation::retire) {
        ++retires;
        cancelled = *request;
        check(!now && !unix_now && !request->content.context && !request->content.ops &&
                  !request->path.data && !request->path.count && !request->snapshot.bytes,
              "preview owner: retirement carries canonical exact key and owner only");
        result.state = AccountMediaRequestState::ready;
        return result;
    }
    if (operation != AccountLocalPreviewOperation::request) return result;
    check(operation == AccountLocalPreviewOperation::request &&
              local_draft_preview_request_valid(*request) && now && unix_now,
          "preview owner: UI admission crosses typed DLL boundary with valid immutable source");
    ++requests;
    submitted = *request;
    result.state = next_result;
    if (next_result == AccountMediaRequestState::queued)
        next_result = AccountMediaRequestState::pending;
    return result;
}
bool geometry(float height) noexcept {
    card = {
        row.id,      row.filename, row.status, {},   row.bytes, 0,    {}, UiAttachmentDraftPhase::uploading,
        row.spoiler, true,         false,      true, false,     false};
    content.attachments = {&card, present ? 1u : 0u};
    return forum_post_update(&scene, content, {0, 0, 640, height}, &measure);
}
AccountMediaFrame receipt() noexcept {
    AccountMediaFrame frame{};
    frame.key = submitted.key;
    frame.source_kind = AccountMediaSourceKind::local_draft_preview;
    frame.source_catalog_scope = submitted.owner_scope;
    frame.source_catalog_generation = submitted.owner_generation;
    frame.source_message_id = submitted.key.id;
    frame.source_message_revision = submitted.key.variant;
    frame.frame_count = 1;
    frame.width = frame.height = 32;
    frame.kind = AccountMediaFrameKind::straight_rgba8;
    return frame;
}
bool uploaded(const AccountMediaFrame& frame) noexcept {
    if (!media_atlas_allocate(&atlas, frame.key, 32, 32).ok())
        return false;
    application_forum_preview_frame_uploaded(frame);
    return application_forum_preview_view(ticket, row.id).ready;
}
void run() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "preview owner: account fixture allocation");
    if (!g_app.account_ui)
        return;
    g_app.account.instance = &actor;
    g_app.plugin_account_scope_serial = 31;
    g_app.account.status.profile_id = 44;
    g_app.account_ui->chat_state.self_user_id = 55;
    g_app.account.api.media_local_preview = media;
    g_app.renderer.preview_media_atlas = &atlas;
    content.heading = utf8_literal("Create a forum post");
    content.context = utf8_literal("Community");
    content.title_label = utf8_literal("Title");
    content.body_label = utf8_literal("Message");
    content.cancel = utf8_literal("Cancel");
    content.accept = utf8_literal("Create post");
    content.add_files = utf8_literal("Add files");
    content.hint = utf8_literal("Ctrl+Enter creates your post.");
    content.editable = content.can_accept = true;
    content.can_add_files = true;
    const bool initialized =
        ui_text_measure_cache_initialize(&measure, {records, 128}, text_measure, nullptr) &&
        media_atlas_initialize(&atlas, 256, 256, 1, {entries, 16}, {buckets, 32}, {allocations, 16},
                               {rows, 16}) &&
        geometry(240);
    check(initialized, "preview owner: production form geometry and preview atlas initialize");
    if (!initialized)
        goto cleanup;
    {
        const u32 before = frames;
        check(application_forum_preview_request(1000, 1) && requests == 0 && frames == before &&
                  !application_forum_preview_next_wake(),
              "preview owner: clipped offscreen card admits no decode and requests no frame");
        check(geometry(1000) && application_forum_preview_request(1100, 1) && requests == 1 &&
                  frames > before,
              "preview owner: actual visible native card admits once and schedules its projection frame");
        auto frame = receipt();
        check(application_forum_preview_frame_current(frame),
              "preview owner: exact actor host source receipt is current");
        auto stale = frame;
        ++stale.source_catalog_generation;
        check(!application_forum_preview_frame_current(stale),
              "preview owner: stale ticket publication rejects completion");
        stale = frame;
        ++stale.source_message_revision;
        check(!application_forum_preview_frame_current(stale),
              "preview owner: stale draft generation rejects completion");
        ++snapshot.identity.last_write;
        check(!application_forum_preview_frame_current(frame),
              "preview owner: changed exact source snapshot rejects completion");
        --snapshot.identity.last_write;
        check(uploaded(frame) && !application_forum_preview_next_wake(),
              "preview owner: atlas upload commits ready projection and clears pending wake");
        const auto warm_frames = frames, warm_requests = requests,
                   warm_revision = application_forum_preview_revision();
        const auto layouts = scene.widgets.layout.revision, measures = measure.backend_calls;
        bool warm = true;
        for (u32 i = 0; i < 64; ++i)
            warm = application_forum_preview_request(1200 + i, 1) && warm;
        check(warm && frames == warm_frames && requests == warm_requests &&
                  application_forum_preview_revision() == warm_revision &&
                  scene.widgets.layout.revision == layouts && measure.backend_calls == measures,
              "preview owner: 64 unchanged ready frames perform zero admission layout text or frame "
              "requests");
        media_atlas_reset(&atlas);
        next_result = AccountMediaRequestState::queued;
        check(application_forum_preview_request(2000, 1) && requests == warm_requests + 1 &&
                  !application_forum_preview_view(ticket, row.id).ready && frames > warm_frames,
              "preview owner: actual atlas eviction readmits exact immutable source without stale ready "
              "state");
        frame = receipt();
        check(uploaded(frame), "preview owner: atlas recovery accepts current generation receipt");
        row.spoiler = true;
        check(geometry(1000), "preview owner: native spoiler card projection");
        media_atlas_reset(&atlas);
        const auto hidden_requests = requests;
        check(application_forum_preview_request(3000, 1) && requests == hidden_requests,
              "preview owner: hidden spoiler preview is never admitted after atlas eviction");
        row.spoiler = false;
        check(geometry(1000), "preview owner: native unhidden card projection");
        next_result = AccountMediaRequestState::failed;
        const auto failure_frames = frames;
        check(application_forum_preview_request(3100, 1) &&
                  application_forum_preview_view(ticket, row.id).failed && frames > failure_frames &&
                  !application_forum_preview_next_wake(),
              "preview owner: decode failure immediately schedules placeholder projection without posting "
              "gate or idle polling");
        const auto failed_requests = requests, failed_frames = frames;
        for (u32 i = 0; i < 64; ++i)
            application_forum_preview_request(3200 + i, 1);
        check(requests == failed_requests && frames == failed_frames,
              "preview owner: 64 unchanged failed placeholders do not retry or schedule frames");
        application_forum_preview_retire(ticket, row.id);
        check(retires == 1 && media_asset_key_equal(cancelled.key, frame.key) &&
                  !application_forum_preview_frame_current(frame) &&
                  !application_forum_preview_key_current(frame.key),
              "preview owner: removed row exact cancellation fences late frame and atlas visibility");
        next_result = AccountMediaRequestState::queued;
        check(application_forum_preview_request(4000, 1), "preview owner: new source lifetime admission");
        frame = receipt();
        check(frame.key.variant != cancelled.key.variant,
              "preview owner: reused row allocates a fresh process preview generation");
        g_app.account.instance = &replacement;
        check(!application_forum_preview_frame_current(frame),
              "preview owner: replaced account actor rejects decoded completion");
        const auto actor_requests = requests;
        application_forum_preview_request(4100, 1);
        check(requests == actor_requests,
              "preview owner: replaced actor never invokes old module callback");
        g_app.account.instance = &actor;
        g_app.account.api.media_local_preview = nullptr;
    }
cleanup:
    application_forum_preview_release();
    forum_post_release(&scene);
    check(application_forum_preview_metrics().owner_bytes == 0,
          "preview owner: lazy optional sidecar releases independently of worker source leases");
    g_app.renderer.preview_media_atlas = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::forum_preview_test
namespace maicord::app_detail {
Application g_app{};
bool application_request_frame() noexcept {
    ++forum_preview_test::frames;
    return true;
}
ApplicationForumPostView application_forum_post_view() noexcept {
    ApplicationForumPostView view{};
    view.ticket = forum_preview_test::ticket;
    view.available = forum_preview_test::valid && g_app.account.instance == &forum_preview_test::actor;
    return view;
}
bool application_forum_post_current(const ApplicationForumPostTicket& ticket) noexcept {
    return forum_preview_test::valid && g_app.account.instance == &forum_preview_test::actor &&
           forum_preview_test::same(ticket, forum_preview_test::ticket);
}
ApplicationForumAttachmentView application_forum_attachments_view(
    const ApplicationForumPostTicket&) noexcept {
    return {
        {&forum_preview_test::row, forum_preview_test::present ? 1u : 0u}, {}, 1, true, false, true, false};
}
bool application_forum_attachments_current(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    return application_forum_post_current(ticket) && forum_preview_test::present &&
           (!id || id == forum_preview_test::row.id);
}
bool application_forum_attachments_source(const ApplicationForumPostTicket& ticket, u64 id,
                                          UploadSource* content, FileSnapshot* snapshot,
                                          ConstSlice<wchar_t>* path) noexcept {
    if (!application_forum_attachments_current(ticket, id))
        return false;
    *content = {};
    *snapshot = forum_preview_test::snapshot;
    *path = {forum_preview_test::path, sizeof(forum_preview_test::path) / sizeof(wchar_t) - 1};
    return true;
}
bool application_forum_form_active() noexcept {
    return forum_preview_test::scene.active;
}
ForumPostScene* application_forum_form_scene() noexcept {
    return &forum_preview_test::scene;
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_application_forum_preview_entry() noexcept {
    maicord::forum_preview_test::run();
    ExitProcess(maicord::forum_preview_test::failures ? 1 : 0);
}

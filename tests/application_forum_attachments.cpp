// Actual forum host and separate media owner, driven through the same typed
// DETAILS and single upload lease boundaries as the shipping application.
// The forum post suite is compiled in and runs alone under --forum-post; with
// no attachment owner the actual media module gives the post host the same
// answers as that suite's own stubs.
#define MAICORD_FORUM_ACTUAL_ATTACHMENTS
#define maicord_application_forum_post_entry maicord_forum_post_mode_entry
#include "application_forum_post.cpp"
#undef maicord_application_forum_post_entry
#include "file_drop_fixture.hpp"
#define clock_monotonic_milliseconds forum_fixture_now
#include "../src/app/application_upload_lane.cpp"
#include "../src/app/application_forum_attachments.cpp"
#undef clock_monotonic_milliseconds
namespace maicord::app_detail {
u64 fixture_limit = 1024;
u64 application_attachment_limit(u64) noexcept { return fixture_limit; }
void application_forum_preview_retire(const ApplicationForumPostTicket&, u64) noexcept {}
}
namespace maicord::forum_media_test {
using namespace app_detail;
using namespace forum_test;
struct MemorySource { u32 references; u8 bytes[8]; } memory{};
void retain(void* value) noexcept { ++static_cast<MemorySource*>(value)->references; }
void release(void* value) noexcept { --static_cast<MemorySource*>(value)->references; }
ConstSlice<u8> read(void* value, u64 offset, Slice<u8> scratch) noexcept {
    if (!scratch.data || !scratch.count) return {};
    auto& source = *static_cast<MemorySource*>(value);
    return offset < 8 ? ConstSlice<u8>{source.bytes + offset, minimum<u32>(scratch.count, static_cast<u32>(8 - offset))}
                      : ConstSlice<u8>{};
}
const UploadSourceOps source_ops{retain, release, read};
struct Upload {
    u64 id; u32 starts, cancels; bool join = true, fail_start, complete;
    UploadSource source;
} upload{};
AccountUploadResult WINAPI upload_api(void*, AccountUploadOperation operation, u64 id,
    const AccountUploadRequest* request) noexcept {
    AccountUploadResult result{}; result.id = id;
    if (operation == AccountUploadOperation::start) {
        check(!upload.id && request && upload_source_valid(request->content), "forum media: one retained worker receives actual owned source");
        upload.id = id; ++upload.starts;
        if (request) { upload.source = request->content; upload_source_retain(upload.source); }
        result.submit_error = upload.fail_start ? HttpsFileUploadError::memory : HttpsFileUploadError::none;
    } else if (operation == AccountUploadOperation::cancel) ++upload.cancels;
    else if (operation == AccountUploadOperation::release && upload.id == id && upload.join) {
        upload_source_release(upload.source); upload.source = {}; upload.id = 0; result.released = true;
    }
    result.owns_transfer = upload.id == id;
    result.transfer.phase = upload.complete ? HttpsFileUploadPhase::complete : HttpsFileUploadPhase::sending;
    result.transfer.status_code = upload.complete ? 200 : 0;
    result.transfer.committed_bytes = upload.complete ? 8 : 3;
    return result;
}
void permit_files() noexcept {
    permit(kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessages | kPermissionAttachFiles);
}
void clean() noexcept {
    upload.join = true;
    if (accepted) receipt(accepted, 500);
    application_forum_post_release(); application_forum_attachments_release();
    check(!forum_owner && !forum_attachments && !upload.id && application_upload_lane_available(),
        "forum media: owners and exact worker lease release together");
    reset_owner(); permit_files(); fixture_limit = 1024; upload.complete = upload.fail_start = false;
}
ApplicationForumPostTicket intent(u64 request) noexcept {
    clean(); auto ticket = begin();
    check(load(request), "forum media: fresh parent metadata"); return ticket;
}
u64 stage(const ApplicationForumPostTicket& ticket, u32 count = 1) noexcept {
    check(application_forum_attachments_begin(ticket), "forum media: lazy separate draft owner");
    MessageAttachmentFile files[10]{}; u64 ids[10]{};
    for (u32 i = 0; i < count; ++i) files[i] = {{}, {{}, 8}, {L"a.txt", 5}, {&memory, &source_ops, 8}};
    check(message_attachment_drafts_add(&forum_attachments->drafts, ticket.parent_id, {files, count}, {ids, 10}) == MessageAttachmentDraftError::none,
        "forum media: bounded memory sources retained by separate store");
    application_forum_post_attachment_changed(ticket); return ids[0];
}
Utf8View url_body() noexcept {
    body_bytes = 0; append("{\"attachments\":[{\"id\":\""); number(submitted.upload_id);
    append("\",\"upload_url\":\"https://upload.example.com/file\",\"upload_filename\":\"fixture/a.txt\"}]}");
    return {body, body_bytes};
}
void finish_upload(u64 request) noexcept {
    check(application_forum_post_pending(now) && application_forum_post_submit(request, now) &&
        submitted.operation == ForumOperation::attachment_url && submitted.upload_bytes == 8,
        "forum media: parent-specific URL uses sole DETAILS flight");
    check(receipt(request, 200, url_body()) && upload.id && !application_upload_lane_available(),
        "forum media: URL response retains the sole PUT lease");
    upload.complete = true; application_forum_attachments_pump(); upload.complete = false;
    check(!upload.id && application_upload_lane_available(), "forum media: successful PUT joins before next admission");
}
Utf8View rich_post(u64 id) noexcept {
    post(id); body_bytes -= 2; // Replace the nested message/object closing braces.
    append(",\"attachments\":[");
    for (u32 i = 0; i < submitted.attachments.count; ++i) {
        if (i) append(",");
        append("{\"id\":\""); number(7000 + i);
        append("\",\"filename\":\"a.txt\",\"size\":8,\"url\":\"https://cdn.discordapp.com/attachments/70/7000/a.txt\",\"description\":\"Diagram\"}");
    }
    append("]}}"); return {body, body_bytes};
}
void run() noexcept {
    check(initialize(), "forum media: actual Core and timeline initialize");
    if (!g_app.account_ui) return;
    g_app.account.api.upload = upload_api;
    static MessageRichStore rich_store{};
    static MessageRichRecord rich_rows[32]{}; static u16 rich_order[32]{}; static u8 rich_text[65536]{};
    check(message_rich_store_initialize(&rich_store, {rich_rows, 32}, {rich_order, 32}, {rich_text, sizeof(rich_text)}) &&
        message_timeline_bind_rich_store(&g_app.message_view.timeline, &rich_store),
        "forum media: actual rich starter publication store");
    auto ticket = intent(100); u64 id = stage(ticket);
    check(!g_app.attachments && memory.references == 1 && !application_forum_post_view().can_submit &&
        application_forum_attachments_describe(ticket, id, utf8_literal("Diagram")),
        "forum media: ordinary composer drafts remain untouched and upload blocks posting");
    finish_upload(101);
    check(application_forum_post_draft_update(ticket, utf8_literal("File only"), {}) &&
        application_forum_post_draft_submit(ticket) && application_forum_post_submit(102, now) &&
        submitted.attachments.count == 1 && !submitted.content.bytes &&
        !application_forum_attachments_remove(ticket, id) &&
        utf8_equal(submitted.attachments[0].description, utf8_literal("Diagram")),
        "forum media: file-only atomic POST freezes owned reference metadata");
    check(receipt(102, 201, rich_post(3000)), "forum media: strict atomic rich receipt accepted");
    const auto* rich = message_rich_store_find(rich_store, 4000);
    check(row(3000) && navigated == 3000 && rich && rich->attachment_count == 1 && rich->attachments[0].id == 7000,
        "forum media: real navigation publishes authoritative attachment ID into rich timeline");
    application_forum_attachments_release();
    check(!memory.references, "forum media: success releases local source after timeline owns strings");

    ticket = intent(110); id = stage(ticket, 10);
    for (u32 i = 0; i < 10; ++i) finish_upload(111 + i);
    UploadedAttachment refs[10]{}; u64 sizes[10]{}; u32 count{};
    check(application_forum_attachments_prepare(ticket, {refs, 10}, {sizes, 10}, &count) && count == 10 &&
        !application_forum_attachments_view(ticket).can_add, "forum media: ten sequential uploads share one worker and all ten refs are retained");
    check(application_forum_post_draft_update(ticket, utf8_literal("Ten files"), {}) && application_forum_post_draft_submit(ticket) &&
        application_forum_post_submit(125, now) && submitted.attachments.count == 10,
        "forum media: ten refs admitted atomically");
    check(receipt(125, 0, {}, 0, RestClientError::timeout) && application_forum_post_view().uncertain &&
        !application_forum_post_retry(ticket) && memory.references == 10 && !application_forum_post_pending(now),
        "forum media: ambiguous POST retains sources and never automatically reuploads or repeats");
    check(application_forum_post_retry(ticket, true) && load(126) && application_forum_post_pending(now),
        "forum media: explicit risk acknowledgment refreshes metadata and staged upload references");

    ticket = intent(130); id = stage(ticket);
    check(application_forum_post_submit(131, now) && receipt(131, 429, {}, now + 500) &&
        application_forum_attachments_retry(ticket, id) && !application_forum_post_pending(now) &&
        application_forum_post_next_wake() == now + 500,
        "forum media: file retry remains editing and obeys actor 429 floor");
    now += 500; finish_upload(132);

    ticket = intent(140); id = stage(ticket); upload.join = false;
    application_forum_post_submit(141, now); receipt(141, 200, url_body());
    check(application_forum_attachments_remove(ticket, id) && upload.cancels && memory.references == 1 &&
        !application_upload_lane_available() && !application_forum_attachments_view(ticket).ready,
        "forum media: removing a PUT releases only draft source while worker drains independently");
    check(!application_forum_attachments_release() && forum_attachments, "forum media: shutdown never blocks or frees retained worker source");
    g_app.account.status.running = false;
    check(!application_forum_attachments_release() && memory.references == 1,
        "forum media: disconnected status is not proof of stopped worker ownership");
    g_app.account.status.running = true; upload.join = true; application_forum_attachments_release();
    check(!memory.references, "forum media: exact release acknowledgement frees retained worker source");

    ticket = intent(150); id = stage(ticket); upload.fail_start = true; upload.join = false;
    application_forum_post_submit(151, now); receipt(151, 200, url_body());
    check(forum_attachments->active_id == id && forum_attachments->finishing && !application_upload_lane_available() &&
        application_forum_attachments_next_wake(), "forum media: retained failed start receives timed nonblocking reap");
    upload.join = true; application_forum_attachments_pump();
    check(!upload.id && application_upload_lane_available() && memory.references == 1,
        "forum media: failed start releases transfer after join but preserves retryable draft");

    ticket = intent(160); id = stage(ticket); application_forum_post_submit(161, now);
    permit(); application_forum_attachments_synchronize();
    const auto withdrawn_revision = application_forum_attachments_view(ticket).revision;
    const auto withdrawn_frames = frames;
    application_forum_attachments_synchronize(); application_forum_attachments_synchronize();
    check(application_forum_attachments_view(ticket).revision == withdrawn_revision && frames == withdrawn_frames,
        "forum media: unchanged permission withdrawal does not continually dirty native frames");
    check(receipt(161, 200, url_body()) && !upload.id && !application_forum_post_draft_submit(ticket),
        "forum media: ATTACH_FILES withdrawal rejects accepted URL before PUT or POST");
    permit_files();
    ApplicationUploadLease lease{};
    check(application_upload_lane_acquire(ApplicationUploadOwner::composer, &lease), "forum media: ordinary composer can reserve same lane");
    auto forged = lease; ++forged.profile;
    ForumPostRequest blocked_upload{}; u64 blocked_draft{};
    check(!application_upload_lane_release(&forged) && !application_upload_lane_available() &&
        !application_forum_attachments_url_candidate(ticket, now, &blocked_upload, &blocked_draft),
        "forum media: foreign owner/profile cannot release lease or admit forum upload");
    check(application_upload_lane_release(&lease), "forum media: unused composer reservation releases without imaginary worker query");

    ticket = intent(165); id = stage(ticket); application_forum_post_submit(166, now);
    const auto valid_url = url_body(); RestClientResponse short_receipt{};
    short_receipt.request_id = 166; short_receipt.status_code = 200; short_receipt.body_bytes = valid_url.bytes + 1;
    accepted = 0;
    check(application_forum_post_response(short_receipt, valid_url) && !upload.id && application_upload_lane_available() &&
        application_forum_attachments_view(ticket).rows[0].phase == MessageAttachmentDraftPhase::failed,
        "forum media: incomplete declared URL receipt drains without starting PUT");

    ticket = intent(167); id = stage(ticket); finish_upload(168); permit();
    check(!application_forum_attachments_view(ticket).ready && !application_forum_post_view().can_submit,
        "forum media: ready files lose submit capability after attach permission withdrawal");

    ticket = intent(170);
    wchar_t path[MAX_PATH]{}; u32 units{}; FileSnapshot snapshot{};
    if (file_drop_fixture::seed(path, &units, &snapshot)) {
        const ConstSlice<wchar_t> dropped{path, units};
        check(application_forum_attachments_drop(ticket, file_drop_fixture::make({&dropped, 1})) &&
            forum_attachments && file_drop_fixture::await(&forum_attachments->picker), "forum media: actual native drop worker prepares original file");
        application_forum_attachments_pump();
        const auto view = application_forum_attachments_view(ticket);
        UploadSource borrowed{}; FileSnapshot retained{}; ConstSlice<wchar_t> copied{};
        check(view.rows.count == 1 && application_forum_attachments_source(ticket, view.rows[0].id, &borrowed, &retained, &copied) &&
            retained.bytes == snapshot.bytes && file_identity_equal(retained.identity, snapshot.identity),
            "forum media: native drop preserves parent-local captured file identity");
        check(file_drop_fixture::reap(&forum_attachments->picker), "forum media: native drop worker joins within fixture bound");
        application_forum_attachments_release(); DeleteFileW(path);
    } else check(false, "forum media: real drop fixture creation");

    clean(); row(70)->type = 16; ++row(70)->revision;
    ticket = begin();
    check(application_forum_post_submit(180, now) && receipt(180, 200,
        utf8_literal("{\"id\":\"70\",\"guild_id\":\"7\",\"type\":16,\"name\":\"Media\",\"flags\":0,\"available_tags\":[]}")) &&
        draft(ticket) && application_forum_post_draft_submit(ticket) && application_forum_post_submit(181, now) &&
        !submitted.attachments.count && receipt(181, 201, post(3010)),
        "forum media: actual type16 metadata and atomic post have no fictional required-file policy");
    clean(); row(70)->type = 15; ++row(70)->revision;

    ticket = intent(185); id = stage(ticket); finish_upload(186);
    draft(ticket); application_forum_post_draft_submit(ticket); application_forum_post_submit(187, now);
    application_forum_post_message_deleted(3020, 4020);
    check(receipt(187, 201, rich_post(3020)) && navigated == 3020 &&
        !message_store_find(g_app.message_view.store, 4020).ok() && !message_rich_store_find(rich_store, 4020),
        "forum media: fresher offselected starter delete wins over old HTTP rich snapshot");

    ticket = intent(190); id = stage(ticket); upload.join = false;
    application_forum_post_submit(191, now); receipt(191, 200, url_body()); fixture_limit = 4;
    application_forum_attachments_synchronize();
    check(forum_attachments->finishing && application_forum_attachments_view(ticket).rows[0].phase == MessageAttachmentDraftPhase::failed &&
        !application_forum_attachments_view(ticket).ready && !application_upload_lane_available(),
        "forum media: quota reduction cancels active transfer and prevents stale ready publication");
    upload.join = true; application_forum_attachments_pump(); fixture_limit = 1024;

    ticket = intent(195); id = stage(ticket); application_forum_post_submit(196, now);
    ++g_app.account.status.profile_id; application_forum_post_synchronize(); application_forum_attachments_synchronize();
    check(receipt(196, 200, url_body()) && !upload.id && !forum_attachments && !memory.references,
        "forum media: profile replacement drains late URL and rejects stale transfer/navigation");

    ticket = intent(197); id = stage(ticket); application_forum_post_submit(198, now);
    row(70)->type = 16; ++row(70)->revision;
    application_forum_post_synchronize(); application_forum_attachments_synchronize();
    check(receipt(198, 200, url_body()) && !upload.id && !forum_attachments && !memory.references,
        "forum media: same-generation parent-type replacement rejects late URL and source publication");
    row(70)->type = 15; ++row(70)->revision;
    ticket = intent(200); id = stage(ticket); upload.join = false;
    application_forum_post_submit(201, now); receipt(201, 200, url_body());
    g_app.window = CreateWindowExW(0, L"STATIC", L"forum deferred close fixture", 0,
        0, 0, 40, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(g_app.window && application_forum_attachments_defer_close() && forum_attachments,
        "forum media: requested window close keeps retained transfer alive until exact join");
    upload.join = true; application_forum_attachments_pump();
    MSG close_message{};
    check(!forum_attachments && IsWindow(g_app.window) &&
        PeekMessageW(&close_message, g_app.window, WM_CLOSE, WM_CLOSE, PM_REMOVE),
        "forum media: completed retained close posts centralized WM_CLOSE instead of bypassing other owners");
    if (g_app.window) DestroyWindow(g_app.window); g_app.window = nullptr;
    clean();
    check(!memory.references && !application_forum_attachments_metrics().committed_bytes,
        "forum media: zero retained source references or lazy media pages after teardown");
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr;
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_application_forum_attachments_entry() noexcept {
    constexpr wchar_t mode[] = L"--forum-post";
    for (const wchar_t* line = GetCommandLineW(); *line; ++line) {
        maicord::u32 matched = 0;
        while (mode[matched] && line[matched] == mode[matched])
            ++matched;
        if (!mode[matched])
            maicord_forum_post_mode_entry();
    }
    maicord::forum_media_test::run(); ExitProcess(maicord::forum_test::failures ? 1 : 0);
}

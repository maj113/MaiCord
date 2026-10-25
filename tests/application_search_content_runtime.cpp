#include "application_state.hpp"
#include "application_search_content.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::search_content_test {
bool target_current(const UiSearchScene&, UiSearchTarget) noexcept;
MessageScene* native_scene(UiSearchScene*) noexcept;
}
#define ui_search_scene_target_current ::maicord::search_content_test::target_current
#define ui_search_scene_message_scene ::maicord::search_content_test::native_scene
#include "../src/app/application_search_content.cpp"
#undef ui_search_scene_target_current
#undef ui_search_scene_message_scene

namespace maicord::search_content_test {
using namespace app_detail;
enum class Mutation : u8 { none, account, profile, self, permission, publication, release, resolver };
u32 failures{}, launches{}, notices{}, navigations{};
u32 account_refreshes{}, confirmations{};
u32 publication = 1000;
u32 layout_revision = 700;
UiSearchScene scene{};
MessageScene media_scene{};
MessageMediaPolicyResult media_policy{MessageExplicitFilter::show, true};
MessageMediaOwner last_media_owner{};
MessageMediaPolicyResult query_media(const void*, const MessageMediaOwner& owner) noexcept {
    last_media_owner = owner;
    return media_policy;
}
MessageScene* native_scene(UiSearchScene*) noexcept { return &media_scene; }
bool allowed = true, visible_link = true, friend_dm = true, last_trusted_text{};
Mutation mutation{};
u8 account_marker{};
MessageStore bank{};
MessageRichStore rich{};
MessageRecord records[2], live_records[2];
u32 order[2], live_order[2];
u8 text[1024], live_text[1024], rich_text[8192];
MessageRichRecord rich_records[4];
u16 rich_order[2];
char opened[2048]{};
MessageLink navigated{};
bool opened_is(const char* url) noexcept {
    u32 i{}; while (url[i] && opened[i] == url[i]) ++i; return url[i] == opened[i];
}
constexpr auto unsigned_file = utf8_literal("https://cdn.discordapp.com/attachments/101/900/report.pdf");
constexpr auto fresh_file = utf8_literal("https://cdn.discordapp.com/attachments/101/900/report.pdf?ex=ffffffff&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
constexpr auto child_file = utf8_literal("https://cdn.discordapp.com/attachments/777/901/child.pdf?ex=ffffffff&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
ApplicationSearchSourceTicket ticket() noexcept { return {800, publication, 0}; }
UiSearchTarget target(bool snapshot = false) noexcept {
    UiSearchTarget value{}; value.action = UiSearchAction::rich_item; value.channel_id = 101;
    value.message_id = 800; value.source_revision = layout_revision; value.source_index = 0;
    value.rich_kind = MessageSceneRichItemKind::attachment; value.snapshot = snapshot;
    return value;
}
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool seed(Utf8View content = utf8_literal("[masked](https://example.test/accepted)"),
          Utf8View file = fresh_file, u32 flags = 0, u32 child_flags = 0) noexcept {
    application_search_content_release();
    allowed = visible_link = friend_dm = true; mutation = Mutation::none;
    layout_revision = 700;
    media_scene = {};
    g_app.frame_pending = false;
    media_scene.media_policy = {nullptr, query_media, 1};
    media_policy = {MessageExplicitFilter::show, true};
    g_app.plugin_account_scope_serial = 7; g_app.account.status.profile_id = 4;
    g_app.account_ui->chat_state.self_user_id = 3; g_app.account_ui->active_channel_id = 999;
    g_app.account.instance = &account_marker; g_app.account.stop_failed = false; g_app.login.visible = false;
    g_app.media_urls = {};
    if (!message_store_initialize(&bank, {records, 2}, {order, 2}, {text, sizeof(text)}) ||
        !message_rich_store_initialize(&rich, {rich_records, 4}, {rich_order, 2}, {rich_text, sizeof(rich_text)})) return false;
    const auto inserted = message_store_upsert(&bank, {800, 101, 4, 1000, 0, content});
    if (!inserted.ok()) return false;
    records[inserted.handle.slot].revision = publication;
    GatewayMessageAttachment attachment{}; attachment.id = 900; attachment.filename = utf8_literal("report.pdf");
    attachment.url = file; attachment.size = 17;
    attachment.flags = flags;
    auto child = attachment; child.id = 901; child.filename = utf8_literal("child.pdf"); child.url = child_file;
    child.flags = child_flags;
    GatewayMessageEmbed embed{}; embed.title = utf8_literal("Embed title");
    embed.url = utf8_literal("https://example.test/embed");
    auto child_embed = embed; child_embed.url = utf8_literal("https://example.test/child-embed");
    GatewayMessageEvent event{}; event.id = 800;
    event.fields = gateway_message_field_attachments | gateway_message_field_embeds | gateway_message_field_message_reference | gateway_message_field_message_snapshots;
    event.embeds = {&embed, 1};
    event.attachments = {&attachment, 1}; event.reference.type = 1;
    event.reference.channel_id = 777; event.reference.message_id = 888;
    event.snapshot.present = true; event.snapshot.content = utf8_literal("[child](https://example.test/snapshot)");
    event.snapshot.fields = gateway_message_field_content | gateway_message_field_attachments | gateway_message_field_embeds;
    event.snapshot.attachments = {&child, 1};
    event.snapshot.embeds = {&child_embed, 1};
    return message_rich_store_apply(&rich, 800, publication, event).ok();
}
MessageTextHit parsed_link(bool snapshot = false) noexcept {
    const auto found = message_store_find(bank, 800);
    const auto* child = snapshot ? message_rich_store_snapshot(rich, 800) : nullptr;
    const auto source = child ? message_rich_store_string(rich, child->snapshot.content)
                              : message_store_content(bank, found.handle);
    char transformed[1024]{}; u32 scratch[1024]{}; TextStyleSpan styles[128]{};
    MessageMarkdownSemantic semantics[128]{};
    MessageMarkdownOptions options{};
    options.spoiler_revealed = [](void*, u32) noexcept { return true; };
    const auto parsed = message_markdown_parse(source, {transformed, sizeof(transformed)},
        {scratch, 1024}, {styles, 128}, options, {semantics, 128});
    if (!parsed.ok()) return {};
    for (u32 i = 0; i < parsed.semantic_count; ++i) {
        const auto& s = semantics[i];
        if (s.kind == MessageMarkdownSemanticKind::link || s.kind == MessageMarkdownSemanticKind::message_link)
            return {800, s.target, publication, s.source_offset | (snapshot ? 0xc0010000u : 0),
                s.source_bytes, s.kind, true};
    }
    return {};
}
// Mutations land before activation: the themed warning is not modal and
// captures the exact URL it shows, so nothing changes under it.
void mutate() noexcept {
    switch (mutation) {
    case Mutation::account: ++g_app.plugin_account_scope_serial; break;
    case Mutation::profile: ++g_app.account.status.profile_id; break;
    case Mutation::self: ++g_app.account_ui->chat_state.self_user_id; break;
    case Mutation::permission: allowed = false; break;
    case Mutation::publication: ++publication; break;
    case Mutation::release: application_search_content_release(); break;
    case Mutation::resolver:
        for (auto& entry : g_app.media_urls.entries)
            if (entry.refreshed_bytes) entry.refreshed[0] = 'x';
        break;
    default: break;
    }
}
bool target_current(const UiSearchScene&, UiSearchTarget value) noexcept {
    // Scene receipt admission is the UI boundary in this host fixture. Its
    // revision intentionally differs from the immutable content publication.
    return allowed && value.source_revision == layout_revision;
}
bool refreshed() noexcept {
    u8 body[1024]{}; ProtocolWriter writer{{body, sizeof(body)}, 0};
    if (!protocol_writer_append_literal(&writer, "{\"refreshed_urls\":[{\"original\":") ||
        !protocol_writer_append_json_string(&writer, unsigned_file) ||
        !protocol_writer_append_literal(&writer, ",\"refreshed\":") ||
        !protocol_writer_append_json_string(&writer, fresh_file) ||
        !protocol_writer_append_literal(&writer, "}]}")) return false;
    auto* entry = media_url_next(&g_app.media_urls, clock_monotonic_milliseconds());
    if (!entry) return false;
    // Synthetic scheduler admission replaces the account/network boundary;
    // resolver completion and the deferred host pump remain production code.
    g_app.media_urls.pending_slot = static_cast<u32>(entry - g_app.media_urls.entries) + 1;
    entry->state = MediaUrlState::pending;
    RestClientResponse response{}; response.status_code = 200;
    media_url_complete(&g_app.media_urls, response, {reinterpret_cast<const char*>(body), writer.used},
        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000);
    return true;
}
void run() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "Search content fixture allocates isolated synthetic account owner");
    if (!g_app.account_ui) return;
    check(message_store_initialize(&g_app.message_view.store, {live_records, 2}, {live_order, 2}, {live_text, sizeof(live_text)}) &&
        message_store_upsert(&g_app.message_view.store, {800, 999, 3, 1000, 0, utf8_literal("https://evil.test/live")}).ok() && seed(),
        "Search content fixture owns independent accepted and decoy live banks");
    auto link = parsed_link();
    check(link.valid && application_search_content_link(ticket(), link) && launches == 1 &&
        opened_is("https://example.test/accepted") && !last_trusted_text,
        "Search masked link resolves accepted source and leaves through the themed warning as masked text");
    auto before = launches;
    check(application_search_content_link(ticket(), parsed_link(true)) && launches == before + 1 &&
        opened_is("https://example.test/snapshot"),
        "Search snapshot masked link uses child field selector in accepted rich bank");
    seed(utf8_literal("see https://example.test/plain now")); before = launches;
    application_search_content_link(ticket(), parsed_link());
    check(launches == before + 1 && opened_is("https://example.test/plain") && last_trusted_text,
        "Search plain link is trusted text under the markdown rule");
    seed(utf8_literal("see https://example.test/plain now")); friend_dm = false;
    application_search_content_link(ticket(), parsed_link());
    check(!last_trusted_text, "Search plain link in a DM with a non-friend is not trusted text");
    // A captured hit outlives a permission or publication change; both block it.
    const Mutation sync_guards[]{Mutation::permission, Mutation::publication};
    bool fenced = true;
    for (auto guard : sync_guards) {
        seed(); link = parsed_link(); before = launches; mutation = guard; mutate();
        application_search_content_link(ticket(), link);
        fenced = launches == before && fenced;
    }
    check(fenced, "Search link revalidates permission and publication before opening");
    seed(utf8_literal("||[masked](https://example.test/secret)||")); link = parsed_link();
    visible_link = false; before = launches;
    application_search_content_link(ticket(), link);
    check(launches == before, "Search hidden spoiler link is blocked by current rendered semantic authority");
    seed(); before = launches; application_search_content_activate(ticket(), target());
    check(launches == before + 1 && !content_owner.intent.pending && !last_trusted_text,
        "Search current signed original file admits distinct layout and publication receipts without a resolver owner");
    seed({}, fresh_file, kGatewayMediaExplicitFlag, kGatewayMediaExplicitFlag);
    media_policy = {MessageExplicitFilter::blur, true};
    before = launches;
    const auto outer_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0);
    const auto child_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0, true);
    application_search_content_activate(ticket(), target());
    check(launches == before && !content_owner.intent.pending && g_app.frame_pending && !account_refreshes &&
        message_media_revealed(media_scene.media_reveals, outer_key) &&
        !message_media_revealed(media_scene.media_reveals, child_key),
        "Search direct explicit file activation reveals only the outer key without external launch");
    g_app.frame_pending = false;
    application_search_content_activate(ticket(), target());
    check(launches == before + 1 && !g_app.frame_pending && !account_refreshes,
        "Search direct second activation opens the previously revealed original once");
    g_app.frame_pending = false;
    application_search_content_activate(ticket(), target(true));
    check(launches == before + 1 && g_app.frame_pending && !account_refreshes &&
        message_media_revealed(media_scene.media_reveals, child_key) &&
        last_media_owner.channel_id == 101 && last_media_owner.author_id == 0 &&
        last_media_owner.source_channel_id == 777 && last_media_owner.source_message_id == 888 &&
        last_media_owner.forwarded,
        "Search forwarded file has its own reveal key and uses the outer destination policy owner");
    g_app.frame_pending = false;
    application_search_content_activate(ticket(), target(true));
    check(launches == before + 2 && !g_app.frame_pending && !account_refreshes && opened_is(child_file.data),
        "Search forwarded file opens only after its separate reveal gesture");
    seed({}, fresh_file, kGatewayMediaExplicitFlag);
    media_policy = {MessageExplicitFilter::block, true};
    before = launches;
    application_search_content_activate(ticket(), target());
    application_search_content_activate(ticket(), target());
    check(launches == before && !media_scene.media_reveals.count && !content_owner.intent.pending &&
        !g_app.frame_pending,
        "Search direct blocked file never reveals or launches after repeated activation");
    media_policy = {MessageExplicitFilter::blur, false};
    const auto confirmation_before = confirmations;
    application_search_content_activate(ticket(), target());
    check(launches == before && confirmations == confirmation_before + 1 &&
        !media_scene.media_reveals.count && !content_owner.intent.pending && !g_app.frame_pending,
        "Search direct eligibility gate requests confirmation without reveal or external activation");
    seed({}, fresh_file, kGatewayAttachmentSpoilerFlag);
    before = launches;
    application_search_content_activate(ticket(), target());
    check(launches == before && message_media_revealed(media_scene.media_reveals, outer_key),
        "Search direct file respects spoiler flags even with explicit media set to show");
    application_search_content_activate(ticket(), target());
    check(launches == before + 1, "Search direct spoiler file opens on its second gesture");
    const MessageMediaPolicyResult refresh_policies[]{{MessageExplicitFilter::blur, true},
        {MessageExplicitFilter::blur, false}};
    bool refresh_fenced = true;
    for (const auto policy : refresh_policies) {
        seed({}, unsigned_file, kGatewayMediaExplicitFlag);
        application_search_content_activate(ticket(), target());
        const bool pending = content_owner.intent.pending && refreshed();
        media_policy = policy;
        before = launches; const auto confirms = confirmations;
        g_app.frame_pending = false;
        application_search_content_pump();
        refresh_fenced = pending && launches == before && confirmations == confirms && !g_app.frame_pending &&
            !media_scene.media_reveals.count && !content_owner.intent.pending && refresh_fenced;
    }
    check(refresh_fenced,
        "Search deferred file cancels newly obscured media without async reveal or eligibility confirmation");
    seed();
    auto embed_target = target(); embed_target.rich_kind = MessageSceneRichItemKind::embed;
    before = launches; application_search_content_activate(ticket(), embed_target);
    check(launches == before + 1 && opened_is("https://example.test/embed") && !last_trusted_text,
        "Search embed card title URL opens exact outer descriptor through the themed warning");
    embed_target.snapshot = true; before = launches;
    application_search_content_activate(ticket(), embed_target);
    check(launches == before + 1 && opened_is("https://example.test/child-embed"),
        "Search forwarded embed URL resolves independent child descriptor");
    fenced = true;
    for (auto guard : sync_guards) {
        seed(); before = launches; mutation = guard; mutate();
        application_search_content_activate(ticket(), target());
        fenced = launches == before && fenced;
    }
    check(fenced, "Search file revalidates permission and publication before opening");
    // A deferred (refreshing) attachment opens only for the account, profile,
    // user, permission and publication it was requested under.
    const Mutation deferred_guards[]{Mutation::account, Mutation::profile, Mutation::self, Mutation::permission,
        Mutation::publication};
    fenced = true;
    for (auto guard : deferred_guards) {
        seed({}, unsigned_file); application_search_content_activate(ticket(), target());
        const bool resolved = content_owner.intent.pending && refreshed();
        before = launches; mutation = guard; mutate(); application_search_content_pump();
        fenced = resolved && launches == before && !content_owner.intent.pending && fenced;
    }
    check(fenced, "Search deferred file revalidates account profile self permission and publication before opening");
    seed({}, unsigned_file); before = launches; const auto notice_before = notices;
    application_search_content_activate(ticket(), target());
    application_search_content_pump(); application_search_content_pump();
    check(content_owner.intent.pending && launches == before && notices == notice_before + 1 && refreshed(),
        "Search unsigned file owns one resolver receipt and unchanged pending pumps do not open or republish notices");
    ++layout_revision;
    before = launches;
    application_search_content_pump();
    check(launches == before + 1 && !content_owner.intent.pending &&
        opened_is("https://cdn.discordapp.com/attachments/101/900/report.pdf?ex=ffffffff&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        "Search signed refresh survives layout revision change and opens the refreshed URL once");
    mutation = Mutation::resolver; mutate(); before = launches; application_search_content_pump();
    check(launches == before, "Search consumed resolver intent never reopens after its arena is reused");
    seed({}, unsigned_file); application_search_content_activate(ticket(), target());
    allowed = false; before = launches; application_search_content_pump();
    check(!content_owner.intent.pending && launches == before,
        "Search permission retirement cancels late deferred Open without OS activation");
    seed({}, unsigned_file); application_search_content_activate(ticket(), target());
    mutation = Mutation::release; mutate(); before = launches; application_search_content_pump();
    check(!content_owner.intent.pending && launches == before, "Search explicit release retires a pending attachment intent");
    seed({}, unsigned_file); application_search_content_activate(ticket(), target());
    application_search_content_link(ticket(), parsed_link(true));
    check(!content_owner.intent.pending, "Search explicit link activation retires prior pending attachment intent");
    seed(utf8_literal("[internal](https://discord.com/channels/22/333/444)"));
    before = launches; const auto nav_before = navigations;
    application_search_content_link(ticket(), parsed_link());
    check(launches == before && navigations == nav_before + 1 && navigated.guild_id == 22 &&
        navigated.channel_id == 333 && navigated.message_id == 444,
        "Search internal masked message link delegates validated target independently of accepted source owner");
    seed(); auto stale = target(); ++stale.source_revision; before = launches;
    application_search_content_activate(ticket(), stale);
    stale = target(); ++stale.source_index; application_search_content_activate(ticket(), stale);
    check(launches == before, "Search attachment actions reject stale layout receipt and result index before opening");
    check(!account_refreshes, "Search content activation never refreshes the unrelated account timeline");
    application_search_content_release();
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr;
}
} // namespace maicord::search_content_test

namespace maicord::app_detail {
bool refresh_account_frame(bool) noexcept { ++search_content_test::account_refreshes; return true; }
void application_user_settings_request_content_confirmation() noexcept { ++search_content_test::confirmations; }
UiSearchScene* application_search_scene() noexcept { return &search_content_test::scene; }
bool application_search_source_resolve(const ApplicationSearchSourceTicket& id, bool snapshot,
                                       ApplicationSearchSourceView* output) noexcept {
    using namespace search_content_test;
    if (!output || !allowed || id.message_id != 800 || id.publication_revision != publication || id.index) return false;
    const auto found = message_store_find(bank, 800);
    const auto* record = found.ok() ? message_store_resolve(bank, found.handle) : nullptr;
    const auto* content = snapshot ? message_rich_store_snapshot(rich, 800) : message_rich_store_find(rich, 800);
    if (!record || !content) return false;
    *output = {id, &bank, record, found.handle, &rich, content, {}, {}, snapshot ? 777ull : 0ull, 22, snapshot};
    return true;
}
bool application_search_source_notice(const ApplicationSearchSourceTicket&, Utf8View) noexcept {
    ++search_content_test::notices; return true;
}
bool application_search_source_link_current(const ApplicationSearchSourceTicket&, const MessageTextHit&) noexcept {
    return search_content_test::visible_link;
}
bool application_link_open(Utf8View url, bool trusted_text) noexcept {
    using namespace search_content_test;
    ++launches; last_trusted_text = trusted_text;
    u32 i{}; for (; i < url.bytes && i < 2047; ++i) opened[i] = url.data[i];
    opened[i] = 0;
    return true;
}
bool application_link_channel_trusted(u64) noexcept { return search_content_test::friend_dm; }
bool application_search_navigation_link(const ApplicationSearchSourceTicket&, const MessageLink& target) noexcept {
    ++search_content_test::navigations; search_content_test::navigated = target; return true;
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_application_search_content_runtime_entry() noexcept {
    maicord::search_content_test::run();
    constexpr char pass[] = "PASS Search content runtime\n";
    constexpr char fail[] = "FAIL Search content runtime\n";
    DWORD written{}; const bool failed = maicord::search_content_test::failures != 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}

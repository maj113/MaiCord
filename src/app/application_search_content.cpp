#include "application_search_content.hpp"
#include "application_link_trust.hpp"
#include "application_search_navigation.hpp"
#include "application_state.hpp"
#include "maicord/app/received_attachment_open.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail {
namespace {
struct SearchContentRequest {
    ApplicationSearchSourceTicket ticket{};
    UiSearchTarget target{};
    u64 account{}, profile{}, self{};
};
struct SearchContentOwner {
    SearchContentRequest request{};
    ReceivedAttachmentOpen intent{};
    u64 generation{};
} content_owner{};
static_assert(sizeof(SearchContentOwner) <= 192);

u64 account_scope() noexcept { return g_app.plugin_account_scope_serial; }
bool resolve(const SearchContentRequest& request, bool snapshot,
             ApplicationSearchSourceView* view) noexcept {
    return view && g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
        !g_app.login.visible && request.account && request.account == account_scope() &&
        request.profile == g_app.account.status.profile_id &&
        request.self == g_app.account_ui->chat_state.self_user_id &&
        application_search_source_resolve(request.ticket, snapshot, view) &&
        view->message && view->store && view->message->id == request.ticket.message_id &&
        view->message->revision == request.ticket.publication_revision;
}
SearchContentRequest capture_request(const ApplicationSearchSourceTicket& ticket, UiSearchTarget target = {}) noexcept {
    return {ticket, target, account_scope(), g_app.account.status.profile_id,
        g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0};
}
void notice(const SearchContentRequest& owner, Utf8View text) noexcept {
    application_search_source_notice(owner.ticket, text);
}
bool attachment_source(const SearchContentRequest& owner,
                       const ReceivedAttachmentOpenIdentity& identity, Utf8View* source) noexcept {
    ApplicationSearchSourceView view{};
    if (!source || !resolve(owner, owner.target.snapshot, &view) || !view.rich || !view.rich_store ||
        identity.account_scope != owner.account || identity.message_id != owner.ticket.message_id ||
        identity.message_revision != owner.ticket.publication_revision ||
        identity.channel_id != view.message->channel_id || identity.source_channel_id != view.source_channel_id ||
        owner.target.action != UiSearchAction::rich_item ||
        owner.target.rich_kind != MessageSceneRichItemKind::attachment ||
        owner.target.rich_index >= view.rich->attachment_count ||
        view.rich->message_revision != owner.ticket.publication_revision) return false;
    const auto& attachment = view.rich->attachments[owner.target.rich_index];
    if (attachment.id != identity.attachment_id ||
        message_rich_attachment_format(*view.rich_store, attachment) != MediaAttachmentFormat::unknown) return false;
    *source = message_rich_store_string(*view.rich_store, attachment.url);
    return source->data && source->bytes;
}
bool snapshot_hit(const MessageTextHit& hit) noexcept {
    return (hit.source_offset & 0x40000000u) != 0;
}
// trusted_text: the official client's markdown rule; the link's text is its URL (not a
// masked [label](url) link) outside a DM with someone who is not a friend.
bool link_source(const SearchContentRequest& owner, const MessageTextHit& hit, Utf8View* source,
                 bool* trusted_text = nullptr) noexcept {
    ApplicationSearchSourceView view{};
    if (!source || !hit.valid || hit.message_id != owner.ticket.message_id ||
        hit.message_revision != owner.ticket.publication_revision ||
        (hit.kind != MessageMarkdownSemanticKind::link && hit.kind != MessageMarkdownSemanticKind::message_link) ||
        !resolve(owner, snapshot_hit(hit), &view) ||
        !application_search_source_link_current(owner.ticket, hit)) return false;
    *source = message_text_presentation_hit_source(*view.store, view.rich_store, hit);
    if (trusted_text && source->data) {
        const bool masked = (hit.source_offset & 0xFFFFu) >= 2 && source->data[-1] == '(' && source->data[-2] == ']';
        *trusted_text = !masked && application_link_channel_trusted(view.message->channel_id);
    }
    return source->data && source->bytes;
}
bool embed_source(const SearchContentRequest& owner, Utf8View* source) noexcept {
    ApplicationSearchSourceView view{};
    if (!source || !resolve(owner, owner.target.snapshot, &view) || !view.rich || !view.rich_store ||
        owner.target.action != UiSearchAction::rich_item || owner.target.rich_kind != MessageSceneRichItemKind::embed ||
        owner.target.resource != MediaEmbedResource::none || owner.target.save ||
        owner.target.rich_index >= view.rich->embed_count ||
        view.rich->message_revision != owner.ticket.publication_revision) return false;
    *source = message_rich_store_string(*view.rich_store, view.rich->embeds[owner.target.rich_index].url);
    return source->data && source->bytes;
}
// Every result link leaves through the link trust rules and the themed
// "Leaving Discord" warning, which captures the exact URL it shows.
bool confirm(const SearchContentRequest& owner, Utf8View url, const ReceivedAttachmentOpenIdentity* attachment,
             bool trusted_text) noexcept {
    if (!message_markdown_url_valid(url) || url.bytes > 2047) {
        notice(owner, utf8_literal("This link could not be opened."));
        return true;
    }
    if (attachment && attachment_refresh_needed(url, clock_unix_milliseconds() / 1000)) {
        notice(owner, utf8_literal("This attachment's link expired. Select it to retry."));
        return true;
    }
    notice(owner, {});
    application_link_open(url, !attachment && trusted_text);
    return true;
}
void attachment_ready(const SearchContentRequest& owner, const ReceivedAttachmentOpenIdentity& identity,
                      Utf8View ready) noexcept {
    confirm(owner, ready, &identity, false);
}
void attachment_notice(const SearchContentRequest& owner, ReceivedAttachmentOpenResult result) noexcept {
    if (result == ReceivedAttachmentOpenResult::pending)
        notice(owner, utf8_literal("Opening attachment…"));
    else if (result == ReceivedAttachmentOpenResult::invalid || result == ReceivedAttachmentOpenResult::failed)
        notice(owner, utf8_literal("This attachment could not be opened. Select it to retry."));
}
} // namespace

bool application_search_content_activate(const ApplicationSearchSourceTicket& ticket, UiSearchTarget target) noexcept {
    ApplicationSearchSourceView view{};
    const auto owner = capture_request(ticket, target);
    const auto* scene = application_search_scene();
    if (!scene || !ui_search_scene_target_current(*scene, target) ||
        target.action != UiSearchAction::rich_item || target.save || target.source_index != ticket.index ||
        target.message_id != ticket.message_id || !resolve(owner, target.snapshot, &view) ||
        target.channel_id != view.message->channel_id || !view.rich || !view.rich_store) return true;
    if (target.rich_kind == MessageSceneRichItemKind::embed) {
        Utf8View source{};
        if (!embed_source(owner, &source)) return true;
        content_owner.intent = {};
        content_owner.request = {};
        ++content_owner.generation;
        MessageLink link{};
        if (message_markdown_message_link(source, &link)) {
            if (!application_search_navigation_link(ticket, link))
                notice(owner, utf8_literal("This message isn't available on this account."));
            return true;
        }
        return confirm(owner, source, nullptr, false);
    }
    if (target.rich_kind != MessageSceneRichItemKind::attachment ||
        target.rich_index >= view.rich->attachment_count) return true;
    const auto& attachment = view.rich->attachments[target.rich_index];
    const ReceivedAttachmentOpenIdentity identity{owner.account, view.message->channel_id, ticket.message_id,
        attachment.id, ticket.publication_revision, view.source_channel_id};
    Utf8View source{}, ready{};
    if (!attachment_source(owner, identity, &source)) return true;
    content_owner.request = owner;
    ++content_owner.generation;
    const auto result = received_attachment_open_begin(&content_owner.intent, identity, source, &g_app.media_urls,
        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready);
    attachment_notice(owner, result);
    if (result == ReceivedAttachmentOpenResult::ready) attachment_ready(owner, identity, ready);
    return true;
}
bool application_search_content_link(const ApplicationSearchSourceTicket& ticket, const MessageTextHit& hit) noexcept {
    const auto owner = capture_request(ticket);
    Utf8View source{};
    bool trusted_text = false;
    if (!link_source(owner, hit, &source, &trusted_text)) return true;
    content_owner.intent = {};
    content_owner.request = {};
    ++content_owner.generation;
    MessageLink link{};
    if (message_markdown_message_link(source, &link)) {
        if (!application_search_navigation_link(ticket, link))
            notice(owner, utf8_literal("This message isn't available on this account."));
        return true;
    }
    return confirm(owner, source, nullptr, trusted_text);
}
void application_search_content_pump() noexcept {
    if (!content_owner.intent.pending) return;
    const auto owner = content_owner.request;
    const auto identity = content_owner.intent.identity;
    Utf8View source{}, ready{};
    if (!attachment_source(owner, identity, &source)) {
        content_owner.intent = {};
        return;
    }
    const auto result = received_attachment_open_resume(&content_owner.intent, identity, source, &g_app.media_urls,
        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready);
    if (result == ReceivedAttachmentOpenResult::failed || result == ReceivedAttachmentOpenResult::invalid)
        attachment_notice(owner, result);
    if (result == ReceivedAttachmentOpenResult::ready) attachment_ready(owner, identity, ready);
}
void application_search_content_release() noexcept {
    ++content_owner.generation;
    content_owner.intent = {};
    content_owner.request = {};
}
} // namespace maicord::app_detail

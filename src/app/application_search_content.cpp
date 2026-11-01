#include "application_search_content.hpp"
#include "maicord/core/memory.hpp"
#include "application_link_trust.hpp"
#include "application_media_policy.hpp"
#include "application_search_navigation.hpp"
#include "application_state.hpp"
#include "maicord/app/received_attachment_open.hpp"
#include "maicord/core/time.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
struct SearchContentRequest {
    ApplicationSearchSourceTicket ticket{};
    UiSearchTarget target{};
    u64 account{}, profile{}, self{};
};
struct SearchContentOwner {
    // Raw bytes, not a SearchContentRequest member: UiSearchTarget's nonzero defaults would put this
    // owner in .data. Written whole before `intent.pending` can be set and read only while it is.
    alignas(SearchContentRequest) u8 request[sizeof(SearchContentRequest)];
    ReceivedAttachmentOpen intent{};
} content_owner{};
static_assert(sizeof(SearchContentOwner) <= 192);

u64 account_scope() noexcept { return g_app.plugin_account_scope_serial; }
bool resolve(const SearchContentRequest& request, bool snapshot,
             ApplicationSearchSourceView* view) noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
        !g_app.login.visible && request.account && request.account == account_scope() &&
        request.profile == g_app.account.status.profile_id &&
        request.self == g_app.account_ui->chat_state.self_user_id &&
        application_search_source_resolve(request.ticket, snapshot, view) &&
        view->message && view->store && view->message->id == request.ticket.message_id &&
        view->message->revision == request.ticket.publication_revision;
}
SearchContentRequest capture_request(const ApplicationSearchSourceTicket& ticket, UiSearchTarget target) noexcept {
    return {ticket, target, account_scope(), g_app.account.status.profile_id,
        g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0};
}
void notice(const SearchContentRequest& owner, Utf8View text) noexcept {
    application_search_source_notice(owner.ticket, text);
}
bool attachment_media_allowed(const SearchContentRequest& owner, bool activate) noexcept {
    ApplicationSearchSourceView view{}, outer{};
    auto* search = application_search_scene();
    auto* scene = search ? ui_search_scene_message_scene(search) : nullptr;
    if (!scene || !resolve(owner, owner.target.snapshot, &view) || !view.rich || !view.rich_store ||
        owner.target.rich_index >= view.rich->attachment_count || !resolve(owner, false, &outer))
        return false;
    const auto& attachment = view.rich->attachments[owner.target.rich_index];
    const auto media_owner = message_media_owner(*outer.message, outer.rich);
    const auto key = message_media_reveal_key(owner.ticket.message_id, MessageMediaSource::attachment,
        owner.target.rich_index, owner.target.snapshot);
    const bool spoiler = message_rich_attachment_spoiler(*view.rich_store, attachment);
    const bool explicit_media = message_rich_attachment_explicit(attachment);
    if (activate)
        return !application_media_activation_consumed(*scene, media_owner, owner.ticket.message_id,
            key, spoiler, explicit_media);
    // Deferred refresh completion can only open already admitted media. It
    // must never reveal an item or request confirmation without a new gesture.
    return message_scene_media_obscure(*scene, media_owner, owner.ticket.message_id,
        key, spoiler, explicit_media) == MessageMediaObscure::none;
}
bool attachment_source(const SearchContentRequest& owner,
                       const ReceivedAttachmentOpenIdentity& identity, Utf8View* source) noexcept {
    ApplicationSearchSourceView view{};
    if (!resolve(owner, owner.target.snapshot, &view) || !view.rich || !view.rich_store ||
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
bool embed_source(const SearchContentRequest& owner, Utf8View* source) noexcept {
    ApplicationSearchSourceView view{};
    if (!resolve(owner, owner.target.snapshot, &view) || !view.rich || !view.rich_store ||
        owner.target.action != UiSearchAction::rich_item || owner.target.rich_kind != MessageSceneRichItemKind::embed ||
        owner.target.resource != MediaEmbedResource::none || owner.target.save ||
        owner.target.rich_index >= view.rich->embed_count ||
        view.rich->message_revision != owner.ticket.publication_revision) return false;
    *source = message_rich_store_string(*view.rich_store, view.rich->embeds[owner.target.rich_index].url);
    return source->data && source->bytes;
}
// Every result link leaves through the link trust rules and the themed
// "Leaving Discord" warning, which captures the exact URL it shows.
bool confirm(const SearchContentRequest& owner, Utf8View url,
             const ReceivedAttachmentOpenIdentity* attachment) noexcept {
    if (!message_markdown_url_valid(url) || url.bytes > 2047) {
        notice(owner, utf8_literal("This link could not be opened."));
        return true;
    }
    if (attachment && attachment_refresh_needed(url, clock_unix_milliseconds() / 1000)) {
        notice(owner, utf8_literal("This attachment's link expired. Select it to retry."));
        return true;
    }
    notice(owner, {});
    application_link_open(url, false);
    return true;
}
void reset_owner() noexcept {
    reset_value(&content_owner.intent);
    clear_bytes(content_owner.request, static_cast<u32>(sizeof(content_owner.request)));
}
// A message link jumps to the message; any other link goes through confirm().
bool open_link(const ApplicationSearchSourceTicket& ticket, const SearchContentRequest& owner,
               Utf8View source) noexcept {
    reset_owner();
    MessageLink link{};
    if (message_markdown_message_link(source, &link)) {
        if (!application_search_navigation_link(ticket, link))
            notice(owner, utf8_literal("This message isn't available on this account."));
        return true;
    }
    return confirm(owner, source, nullptr);
}
void attachment_ready(const SearchContentRequest& owner, const ReceivedAttachmentOpenIdentity& identity,
                      Utf8View ready) noexcept {
    if (attachment_media_allowed(owner, false))
        confirm(owner, ready, &identity);
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
        return !embed_source(owner, &source) || open_link(ticket, owner, source);
    }
    if (target.rich_kind != MessageSceneRichItemKind::attachment ||
        target.rich_index >= view.rich->attachment_count) return true;
    const auto& attachment = view.rich->attachments[target.rich_index];
    const ReceivedAttachmentOpenIdentity identity{owner.account, view.message->channel_id, ticket.message_id,
        attachment.id, ticket.publication_revision, view.source_channel_id};
    Utf8View source{}, ready{};
    if (!attachment_source(owner, identity, &source)) return true;
    if (!attachment_media_allowed(owner, true)) return true;
    ::new (static_cast<void*>(content_owner.request)) SearchContentRequest(owner);
    const auto result = received_attachment_open_begin(&content_owner.intent, identity, source, &g_app.media_urls,
        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready);
    attachment_notice(owner, result);
    if (result == ReceivedAttachmentOpenResult::ready) attachment_ready(owner, identity, ready);
    return true;
}
void application_search_content_pump() noexcept {
    if (!content_owner.intent.pending) return;
    const SearchContentRequest owner =
        *__builtin_launder(reinterpret_cast<const SearchContentRequest*>(content_owner.request));
    const auto identity = content_owner.intent.identity;
    Utf8View source{}, ready{};
    if (!attachment_source(owner, identity, &source) || !attachment_media_allowed(owner, false)) {
        reset_value(&content_owner.intent);
        return;
    }
    const auto result = received_attachment_open_resume(&content_owner.intent, identity, source, &g_app.media_urls,
        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready);
    if (result == ReceivedAttachmentOpenResult::failed || result == ReceivedAttachmentOpenResult::invalid)
        attachment_notice(owner, result);
    if (result == ReceivedAttachmentOpenResult::ready) attachment_ready(owner, identity, ready);
}
void application_search_content_release() noexcept {
    reset_owner();
}
} // namespace maicord::app_detail

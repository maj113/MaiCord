#include "search_runtime_internal.hpp"
#include "../../../../src/app/shell/content_input.hpp"
#include "application_search_measurement.hpp"

namespace maicord {
bool renderer_bind_search_message_text_presentation(D3D11Renderer* renderer,
                                                    const MessageTextPresentation* p,
                                                    const UserStore* users,
                                                    UiTextMeasureCache* cache) noexcept {
    if (!renderer)
        return false;
    renderer->search_message_text = p;
    renderer->search_message_users = users;
    renderer->search_message_measure_cache = cache;
    return true;
}
void renderer_unbind_search_message_text_presentation(D3D11Renderer* renderer) noexcept {
    if (renderer) {
        renderer->search_message_text = nullptr;
        renderer->search_message_users = nullptr;
        renderer->search_message_measure_cache = nullptr;
    }
}
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++search_runtime_test::overlay_repaints;
    return true;
}
TextSystem renderer_text_system(D3D11Renderer*, UiTextFace) noexcept {
    return search_runtime_test::search_measurement_text_system();
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position) {
        return false;
    }
    *position = search_runtime_test::hit_position;
    return true;
}
namespace app_detail {
MessageMediaPolicy application_message_media_policy() noexcept {
    return {nullptr, [](const void*, const MessageMediaOwner&) noexcept {
        return search_runtime_test::media_policy;
    }, 1};
}
void application_user_settings_request_content_confirmation() noexcept {
    ++search_runtime_test::content_confirmations;
}
bool refresh_account_frame(bool force) noexcept { return refresh_account_text(force); }
bool application_poll_search_state(u64 id, u32 publication, Slice<UiPollAnswer>, UiPollState*,
                                   ApplicationPollSource*) noexcept {
    ++search_runtime_test::poll_projection_calls;
    search_runtime_test::projected_poll_id = id;
    search_runtime_test::projected_poll_publication = publication;
    return false;
}
bool application_downloads_active() noexcept {
    return search_runtime_test::downloads;
}
bool application_call_header_search(float, UiRect* field) noexcept {
    if (field)
        *field = {};
    return false; // No call view covers the title bar in this runtime.
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& ticket, u64 attachment,
                                        bool snapshot) noexcept {
    ++search_runtime_test::search_saves;
    search_runtime_test::saved_search_ticket = ticket;
    search_runtime_test::saved_search_attachment = attachment;
    search_runtime_test::saved_search_snapshot = snapshot;
    return true;
}
bool application_image_viewer_open_search(const ApplicationSearchSourceTicket&, MessageSceneRichItemKind,
                                          u32 index, bool, const UiRect& origin, MediaEmbedResource) noexcept {
    ++search_runtime_test::viewer_opens;
    search_runtime_test::viewer_index = index;
    search_runtime_test::viewer_origin = origin;
    return false;
}
bool application_video_open_search(const UiSearchTarget&) noexcept {
    return false;
}
bool application_search_content_activate(const ApplicationSearchSourceTicket&, UiSearchTarget) noexcept {
    return true;
}
void application_search_content_release() noexcept {
    ++search_runtime_test::content_retirements;
}
void application_search_navigation_synchronize() noexcept {
}
void application_search_navigation_release() noexcept {
    ++search_runtime_test::navigation_releases;
}
bool application_search_navigation_begin(const ApplicationSearchSourceTicket& ticket) noexcept {
    ApplicationSearchNavigationSource source{};
    return application_search_navigation_source(ticket, &source) &&
           application_navigate_known_message(source.channel_id, ticket.message_id);
}
void application_pinned_messages_close() noexcept {
}
bool application_thread_browser_active() noexcept {
    return search_runtime_test::browser_open;
}
bool application_thread_browser_search_scope(ApplicationThreadBrowserSearchScope* out) noexcept {
    if (!out || !search_runtime_test::browser_open)
        return false;
    *out = {70, search_runtime_test::browser_session, utf8_literal("fixture-channel"),
            search_runtime_test::browser_forum};
    return true;
}
bool application_thread_browser_search(Utf8View name) noexcept {
    ++search_runtime_test::browser_searches;
    search_runtime_test::browser_query_bytes = minimum<u32>(name.bytes, sizeof(search_runtime_test::browser_query));
    copy_bytes(search_runtime_test::browser_query, name.data, search_runtime_test::browser_query_bytes);
    return true;
}
void application_thread_browser_close() noexcept {
    ++search_runtime_test::browser_closes;
    search_runtime_test::browser_open = false;
}
bool refresh_account_text(bool force) noexcept {
    ++search_runtime_test::repaints;
    search_runtime_test::forced_account_repaints += force ? 1u : 0u;
    return true;
}
bool application_settings_active() noexcept {
    return search_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
const RelationshipStore* application_friends_relationships() noexcept {
    return &search_runtime_test::choice_relationships;
}
bool application_composer_text_input_composing() noexcept {
    return search_runtime_test::composing;
}
void application_member_query_note(u64 guild_id, u64, Utf8View query) noexcept {
    ++search_runtime_test::member_queries;
    search_runtime_test::member_query_active = true;
    search_runtime_test::composer_query_owned = false;
    search_runtime_test::member_query_guild = guild_id;
    search_runtime_test::member_query_bytes =
        minimum<u32>(query.bytes, sizeof(search_runtime_test::member_query));
    copy_bytes(search_runtime_test::member_query, query.data, search_runtime_test::member_query_bytes);
}
void application_member_query_cancel() noexcept {
    ++search_runtime_test::member_query_withdrawals;
    if (!search_runtime_test::composer_query_owned)
        search_runtime_test::member_query_active = false;
}
void application_composer_text_input_suspend() noexcept {
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_synchronize() noexcept {
    return true;
}
bool application_navigate_known_message(u64 channel, u64 message) noexcept {
    ++search_runtime_test::navigations;
    search_runtime_test::navigated_channel = channel;
    search_runtime_test::navigated_message = message;
    return true;
}

} // namespace app_detail
} // namespace maicord

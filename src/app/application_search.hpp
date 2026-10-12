#pragma once
#include "maicord/ui/input_router.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/app/message_timeline.hpp"
#include "maicord/ui/message_text_presentation.hpp"

namespace maicord::app_detail {
struct ApplicationSearch;
// Publication IDs are process-wide, monotonic and never reused. Layout/source
// revision changes do not retire this immutable accepted content identity.
struct ApplicationSearchSourceTicket {
    u64 message_id{};
    u32 publication_revision{}, index{};
};
static_assert(sizeof(ApplicationSearchSourceTicket) == 16);
struct ApplicationSearchSourceView {
    ApplicationSearchSourceTicket ticket{};
    const MessageStore* store{};
    const MessageRecord* message{};
    MessageHandle handle{};
    MessageRichStore* rich_store{};
    const MessageRichRecord* rich{};
    Utf8View author{}, avatar_hash{};
    u64 source_channel_id{};
    u64 guild_id{};
    bool snapshot{};
};
struct ApplicationSearchNavigationSource {
    ApplicationSearchSourceTicket ticket{};
    u64 channel_id{}, guild_id{};
    bool channel_known{};
};
bool application_search_source_ticket(u32 index, ApplicationSearchSourceTicket*) noexcept;
// Valid while the Search panel remains open beneath viewer/download UI.
// Explicit Search close, a new query/page, accepted publication replacement,
// source/account/profile/scope retirement and permission loss revoke authority.
// Strings/records remain borrowed only until that authority changes. Accepted
// content is immutable; rich_store permits existing exact-revision dimension
// setters for decoded media, never URL/content mutation.
bool application_search_source_resolve(const ApplicationSearchSourceTicket&, bool snapshot,
                                       ApplicationSearchSourceView*) noexcept;
bool application_search_source_resolve(u64 message_id, u32 publication_revision, bool snapshot,
                                       ApplicationSearchSourceView*) noexcept;
MessageTimeline* application_search_rich_timeline() noexcept;
UiSearchScene* application_search_scene() noexcept;
MessageTextPresentation* application_search_message_text_presentation() noexcept;
void application_search_theme_changed() noexcept;
void application_search_media_dimensions_changed() noexcept;
// Jump alone may hydrate an accepted guild result whose channel is not yet in
// the native model. This does not authorize media/file actions before hydration.
bool application_search_navigation_source(const ApplicationSearchSourceTicket&,
                                          ApplicationSearchNavigationSource*) noexcept;
// Feedback is scoped to the exact still-current accepted publication. The
// caller supplies a persistent/static string; HTTP/decoder views are forbidden.
bool application_search_source_notice(const ApplicationSearchSourceTicket&, Utf8View) noexcept;
bool application_search_active() noexcept;
bool application_search_close_panel() noexcept;
bool application_search_read_blocked() noexcept;
bool application_search_synchronize() noexcept;
void application_search_release() noexcept;
bool application_search_input(UiInputKind, u32, bool repeated) noexcept;
bool application_search_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_search_pending(u64 now_ms) noexcept;
bool application_search_submit(u64 request_id, u64 now_ms) noexcept;
bool application_search_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_search_next_wake() noexcept;
u64 application_search_edit_next_wake() noexcept;
void application_search_tick(u64 now_ms) noexcept;
MessageComposer* application_search_edit_model() noexcept;
bool application_search_edit_focused() noexcept;
TextInputTarget application_search_edit_target() noexcept;
UiRect application_search_edit_bounds() noexcept;
bool application_search_edit_changed(bool content) noexcept;
void application_search_edit_cancel_native_group(const TextInputTarget&) noexcept;
} // namespace maicord::app_detail

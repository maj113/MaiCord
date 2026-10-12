#include "maicord/app/reaction_viewer_controller.hpp"
#include "maicord/core/deadline.hpp"

#include <Windows.h>

namespace maicord {
namespace {

u64 next_epoch(u64 epoch) noexcept {
    return epoch == ~0ull ? 1 : epoch + 1;
}

ReactionViewerControllerResult failure(ReactionViewerController* controller,
                                       ReactionViewerControllerError error, bool changed = false,
                                       bool retry = false) noexcept {
    saturating_increment_if(controller && error != ReactionViewerControllerError::none,
                            controller->failures);
    return ReactionViewerControllerResult{error, changed, false, retry};
}

void require_current_page(ReactionViewerController* controller) noexcept {
    if (!controller) {
        return;
    }
    controller->retry_at_ms = 0;
    controller->retry_attempt = 0;
    controller->phase = controller->viewer && controller->viewer->active
                            ? ReactionViewerRequestPhase::required
                            : ReactionViewerRequestPhase::idle;
}

ReactionViewerControllerResult stale_response(ReactionViewerController* controller) noexcept {
    saturating_increment(controller->stale_responses);
    reaction_viewer_set_loading(controller->viewer, false);
    require_current_page(controller);
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, true, true, false};
}

ReactionViewerControllerResult retry_or_fail(ReactionViewerController* controller, u64 now_monotonic_ms,
                                             u64 retry_at_ms,
                                             ReactionViewerControllerError terminal_error) noexcept {
    if (!controller || !controller->viewer || !controller->viewer->active) {
        return failure(controller, ReactionViewerControllerError::state);
    }
    if (controller->retry_attempt < kReactionViewerMaximumRetries) {
        ++controller->retry_attempt;
        saturating_increment(controller->retries);
        if (retry_at_ms == 0) {
            const u64 delay = 250ull << (controller->retry_attempt - 1);
            retry_at_ms = deadline_after(now_monotonic_ms, delay);
        }
        controller->retry_at_ms = retry_at_ms;
        controller->phase = ReactionViewerRequestPhase::retry_wait;
        reaction_viewer_set_loading(controller->viewer, true);
        return ReactionViewerControllerResult{ReactionViewerControllerError::none, true, false, true};
    }
    controller->phase = ReactionViewerRequestPhase::idle;
    controller->retry_at_ms = 0;
    reaction_viewer_set_failed(controller->viewer);
    return failure(controller, terminal_error, true);
}

} // namespace

bool reaction_viewer_controller_initialize(ReactionViewerController* controller, ReactionViewer* viewer,
                                           Slice<ReactionUser> page_users) noexcept {
    if (!controller || !viewer || !page_users.data || page_users.count < kReactionUsersMaximumPage) {
        return false;
    }
    *controller = ReactionViewerController{};
    controller->viewer = viewer;
    controller->page_users = page_users.data;
    controller->page_user_capacity = page_users.count;
    controller->epoch = 1;
    controller->initialized = true;
    return true;
}

ReactionViewerControllerResult reaction_viewer_controller_open(ReactionViewerController* controller,
                                                               u64 guild_id, u64 channel_id, u64 message_id,
                                                               const MessageRichStore& rich_store,
                                                               const MessageRichRecord& rich,
                                                               UiRect viewport) noexcept {
    if (!controller || !controller->initialized || !controller->viewer) {
        return failure(controller, ReactionViewerControllerError::invalid_input);
    }
    const ReactionViewerResult opened = reaction_viewer_open(controller->viewer, guild_id, channel_id,
                                                             message_id, rich_store, rich, viewport);
    if (!opened.ok()) {
        return failure(controller, ReactionViewerControllerError::viewer);
    }
    controller->epoch = next_epoch(controller->epoch);
    controller->retry_at_ms = 0;
    controller->retry_attempt = 0;
    if (controller->phase != ReactionViewerRequestPhase::submitted) {
        controller->phase = ReactionViewerRequestPhase::required;
    } else {
        reaction_viewer_set_loading(controller->viewer, true);
    }
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, opened.changed, false,
                                          false};
}

ReactionViewerControllerResult reaction_viewer_controller_close(
    ReactionViewerController* controller) noexcept {
    if (!controller || !controller->initialized || !controller->viewer) {
        return failure(controller, ReactionViewerControllerError::invalid_input);
    }
    bool changed = false;
    if (!reaction_viewer_close(controller->viewer, &changed)) {
        return failure(controller, ReactionViewerControllerError::viewer);
    }
    if (changed) {
        controller->epoch = next_epoch(controller->epoch);
    }
    controller->retry_at_ms = 0;
    controller->retry_attempt = 0;
    if (controller->phase != ReactionViewerRequestPhase::submitted) {
        controller->phase = ReactionViewerRequestPhase::idle;
    }
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, changed, false, false};
}

ReactionViewerControllerResult reaction_viewer_controller_select(ReactionViewerController* controller,
                                                                 u32 aggregate_index) noexcept {
    if (!controller || !controller->initialized || !controller->viewer) {
        return failure(controller, ReactionViewerControllerError::invalid_input);
    }
    const ReactionViewerResult selected =
        reaction_viewer_select_aggregate(controller->viewer, aggregate_index);
    if (!selected.ok()) {
        return failure(controller, ReactionViewerControllerError::viewer);
    }
    if (selected.changed) {
        controller->epoch = next_epoch(controller->epoch);
        controller->retry_at_ms = 0;
        controller->retry_attempt = 0;
        if (controller->phase != ReactionViewerRequestPhase::submitted) {
            controller->phase = ReactionViewerRequestPhase::required;
        } else {
            reaction_viewer_set_loading(controller->viewer, true);
        }
    }
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, selected.changed, false,
                                          false};
}

bool reaction_viewer_controller_prepare_request(ReactionViewerController* controller, u64 now_monotonic_ms,
                                                ReactionViewerQuery* query) noexcept {
    if (!controller || !query || !controller->initialized || !controller->viewer ||
        !controller->viewer->active) {
        return false;
    }
    if (controller->phase == ReactionViewerRequestPhase::retry_wait) {
        if (controller->retry_at_ms == 0 || now_monotonic_ms < controller->retry_at_ms) {
            return false;
        }
        controller->phase = ReactionViewerRequestPhase::required;
        controller->retry_at_ms = 0;
    }
    const ReactionViewer& viewer = *controller->viewer;
    if (controller->phase != ReactionViewerRequestPhase::required ||
        viewer.selected_aggregate >= viewer.aggregate_count) {
        return false;
    }
    const ReactionViewerAggregate& aggregate = viewer.aggregates[viewer.selected_aggregate];
    const u32 remaining =
        viewer.user_count < viewer.user_capacity ? viewer.user_capacity - viewer.user_count : 0;
    if (remaining == 0) {
        return false;
    }
    *query = ReactionViewerQuery{reaction_viewer_aggregate_name(viewer, viewer.selected_aggregate),
                                 viewer.channel_id,
                                 viewer.message_id,
                                 aggregate.emoji_id,
                                 viewer.after_user_id,
                                 controller->epoch,
                                 aggregate.type,
                                 minimum(kReactionUsersMaximumPage, remaining)};
    return query->emoji_name.data && query->emoji_name.bytes != 0;
}

ReactionViewerControllerResult reaction_viewer_controller_mark_submitted(
    ReactionViewerController* controller, u64 epoch) noexcept {
    if (!controller || !controller->initialized || !controller->viewer || !controller->viewer->active ||
        controller->phase != ReactionViewerRequestPhase::required || epoch != controller->epoch) {
        return failure(controller, ReactionViewerControllerError::state);
    }
    controller->request_epoch = epoch;
    controller->phase = ReactionViewerRequestPhase::submitted;
    saturating_increment(controller->requests);
    reaction_viewer_set_loading(controller->viewer, true);
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, true, false, false};
}

ReactionViewerControllerResult reaction_viewer_controller_submit_failed(
    ReactionViewerController* controller, u64 now_monotonic_ms) noexcept {
    if (!controller || !controller->initialized ||
        (controller->phase != ReactionViewerRequestPhase::submitted &&
         controller->phase != ReactionViewerRequestPhase::required)) {
        return failure(controller, ReactionViewerControllerError::state);
    }
    if (controller->phase == ReactionViewerRequestPhase::required) {
        controller->request_epoch = controller->epoch;
    }
    if (controller->request_epoch != controller->epoch || !controller->viewer ||
        !controller->viewer->active) {
        return stale_response(controller);
    }
    return retry_or_fail(controller, now_monotonic_ms, 0, ReactionViewerControllerError::transport);
}

ReactionViewerControllerResult reaction_viewer_controller_accept_response(
    ReactionViewerController* controller, const RestClientResponse& response, ConstSlice<u8> body,
    Slice<u32> page_structural_scratch, Slice<u32> object_structural_scratch) noexcept {
    if (!controller || !controller->initialized || !controller->viewer ||
        controller->phase != ReactionViewerRequestPhase::submitted) {
        return failure(controller, ReactionViewerControllerError::state);
    }
    controller->last_http_status = response.status_code;
    controller->last_transport_error = response.error;
    if (controller->request_epoch != controller->epoch || !controller->viewer->active) {
        return stale_response(controller);
    }
    const u64 now = GetTickCount64();
    if (!response.transport_ok()) {
        return retry_or_fail(controller, now, 0, ReactionViewerControllerError::transport);
    }
    if (response.status_code == 429 && response.retry_at_ms != 0) {
        return retry_or_fail(controller, now, response.retry_at_ms, ReactionViewerControllerError::http);
    }
    if (response.status_code == 408 || response.status_code >= 500) {
        return retry_or_fail(controller, now, 0, ReactionViewerControllerError::http);
    }
    if (response.status_code != 200 || !body.data || body.count == 0) {
        controller->phase = ReactionViewerRequestPhase::idle;
        reaction_viewer_set_failed(controller->viewer);
        return failure(controller, ReactionViewerControllerError::http, true);
    }
    Slice<char> text_output = reaction_viewer_page_text_output(*controller->viewer);
    const ReactionUsersDecodeResult decoded = decode_reaction_users(
        Utf8View{reinterpret_cast<const char*>(body.data), body.count}, page_structural_scratch,
        object_structural_scratch, 16,
        Slice<ReactionUser>{controller->page_users, controller->page_user_capacity}, text_output);
    controller->last_decode_error = decoded.error;
    controller->last_scan_error = decoded.scan_error;
    controller->last_read_error = decoded.read_error;
    controller->last_decode_offset = decoded.error_offset;
    if (!decoded.ok()) {
        controller->phase = ReactionViewerRequestPhase::idle;
        reaction_viewer_set_failed(controller->viewer);
        return failure(controller, ReactionViewerControllerError::decode, true);
    }
    const ReactionViewerResult accepted = reaction_viewer_accept_page(
        controller->viewer, ConstSlice<ReactionUser>{controller->page_users, decoded.user_count},
        decoded.text_bytes);
    if (!accepted.ok()) {
        controller->phase = ReactionViewerRequestPhase::idle;
        reaction_viewer_set_failed(controller->viewer);
        return failure(controller, ReactionViewerControllerError::viewer, true);
    }
    controller->phase = ReactionViewerRequestPhase::idle;
    controller->retry_at_ms = 0;
    controller->retry_attempt = 0;
    saturating_increment(controller->pages);
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, accepted.changed, false,
                                          false};
}

ReactionViewerControllerResult reaction_viewer_controller_request_more(
    ReactionViewerController* controller) noexcept {
    if (!controller || !controller->initialized || !controller->viewer) {
        return failure(controller, ReactionViewerControllerError::invalid_input);
    }
    bool requested = false;
    if (!reaction_viewer_request_more(controller->viewer, &requested)) {
        return failure(controller, ReactionViewerControllerError::viewer);
    }
    if (requested && controller->phase == ReactionViewerRequestPhase::idle) {
        controller->phase = ReactionViewerRequestPhase::required;
    }
    return ReactionViewerControllerResult{ReactionViewerControllerError::none, requested, false, false};
}

u64 reaction_viewer_controller_next_wake_at(const ReactionViewerController& controller) noexcept {
    return controller.initialized && controller.phase == ReactionViewerRequestPhase::retry_wait
               ? controller.retry_at_ms
               : 0;
}

} // namespace maicord

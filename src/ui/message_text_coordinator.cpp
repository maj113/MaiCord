#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

MessageTextCoordinatorResult result(
    MessageTextCoordinatorError error, u8 passes = 0,
    MessageSceneError scene_error = MessageSceneError::none,
    MessageTextPresentationError presentation_error = MessageTextPresentationError::none,
    bool finalized = false) noexcept {
    return MessageTextCoordinatorResult{error, scene_error, presentation_error, passes, finalized};
}

} // namespace

MessageTextCoordinatorResult message_text_coordinator_converge(
    const MessageTextCoordinatorRequest& request) noexcept {
    if (request.trace) {
        request.trace->count = 0;
    }
    if (!request.scene || !request.presentation || !request.measure_cache || !request.state ||
        !request.navigation || !request.theme || !request.finalize) {
        return result(MessageTextCoordinatorError::invalid_input);
    }
    u64 previous_projection = 0;
    u64 previous_scroll_units = 0;
    u64 previous_total_units = 0;
    u32 previous_first = 0;
    u32 previous_count = 0;
    bool have_previous_projection = false;
    for (u32 pass = 0; pass < kMaximumMessageTextConvergencePasses; ++pass) {
        const u8 passes = static_cast<u8>(pass + 1u);
        const MessageSceneResult scene = message_scene_update(request.scene, request.scene->viewport);
        if (!scene.ok()) {
            return result(MessageTextCoordinatorError::scene, passes, scene.error);
        }
        MessageTextPresentation& presentation = *request.presentation;
        if (pass == 0 && request.scroll_refresh && scene.scroll_only && presentation.active &&
            presentation.shift_layout_passes && presentation.shift_layout_passes + 1 == request.scene->layout_passes &&
            (!presentation.next_timestamp_update_unix_seconds ||
             request.now_unix_seconds < presentation.next_timestamp_update_unix_seconds)) {
            message_text_presentation_shift(&presentation, scene.scroll_shift);
            presentation.shift_layout_passes = request.scene->layout_passes;
            presentation.scroll_moving = request.scroll_moving;
            if (!request.finalize(request.finalize_context, *request.theme))
                return result(MessageTextCoordinatorError::finalization, passes);
            return result(MessageTextCoordinatorError::none, passes, MessageSceneError::none,
                          MessageTextPresentationError::none, true);
        }
        const MessageTextPresentationResult projected = message_text_presentation_build(
            request.presentation, *request.scene, *request.state, request.users, *request.navigation,
            *request.theme, request.now_unix_seconds);
        if (!projected.ok()) {
            return result(MessageTextCoordinatorError::projection, passes, MessageSceneError::none,
                          projected.error);
        }
        bool command_changed = false;
        if (!message_text_presentation_measure(request.presentation, request.measure_cache,
                                               request.backend_revision, &command_changed)) {
            return result(MessageTextCoordinatorError::measurement, passes, MessageSceneError::none,
                          request.presentation->last_error);
        }
        bool layout_changed = false;
        if (!message_text_presentation_apply_measurements(request.scene, *request.presentation,
                                                          &layout_changed)) {
            return result(MessageTextCoordinatorError::application, passes, MessageSceneError::none,
                          request.presentation->last_error);
        }
        const UiVariableList* list =
            request.scene->timeline ? request.scene->timeline->list : nullptr;
        const u64 scroll_units =
            request.scene->timeline ? request.scene->timeline->scroll_units : 0;
        const u64 total_units = list ? list->total_units : 0;
        const u64 projection = request.presentation->projection_signature;
        const bool projection_fixed =
            have_previous_projection && !command_changed &&
            projection == previous_projection && scroll_units == previous_scroll_units &&
            total_units == previous_total_units && scene.first == previous_first &&
            scene.count == previous_count;
        if (request.trace) {
            MessageTextCoordinatorPass& trace_pass = request.trace->passes[pass];
            reset_value<MessageTextCoordinatorPass>(&trace_pass);
            trace_pass.scroll_units = scroll_units;
            trace_pass.total_units = total_units;
            trace_pass.first = scene.first;
            trace_pass.count = scene.count;
            trace_pass.command_count = request.presentation->command_count;
            trace_pass.measurement_count = request.presentation->measurement_count;
            for (u32 index = 0; list && index < 4 && index < list->count; ++index) {
                trace_pass.leading_item_units[index] = ui_variable_list_item_units(*list, index);
            }
            trace_pass.flags =
                static_cast<u8>((scene.changed ? message_text_coordinator_scene_changed : 0u) |
                                (command_changed ? message_text_coordinator_command_changed : 0u) |
                                (layout_changed ? message_text_coordinator_layout_changed : 0u) |
                                (message_scene_pending(*request.scene) ? message_text_coordinator_scene_dirty : 0u) |
                                (projection_fixed ? message_text_coordinator_projection_fixed : 0u));
            request.trace->count = passes;
        }
        previous_projection = projection;
        previous_scroll_units = scroll_units;
        previous_total_units = total_units;
        previous_first = scene.first;
        previous_count = scene.count;
        have_previous_projection = true;
        if (layout_changed || command_changed) {
            if (projection_fixed && layout_changed) {
                // Apply the just-accepted retained metrics to widget geometry,
                // then demand that the resulting projection is still exactly
                // the one that reached the fixed point. This prevents stale
                // text without hiding a genuine layout transition.
                const MessageSceneResult relaid = message_scene_update(
                    request.scene, request.scene->viewport);
                if (!relaid.ok()) {
                    return result(MessageTextCoordinatorError::scene, passes,
                                  relaid.error);
                }
                const MessageTextPresentationResult reprojected =
                    message_text_presentation_build(
                        request.presentation, *request.scene, *request.state,
                        request.users, *request.navigation, *request.theme,
                        request.now_unix_seconds);
                if (!reprojected.ok()) {
                    return result(MessageTextCoordinatorError::projection,
                                  passes, MessageSceneError::none,
                                  reprojected.error);
                }
                const u64 reprojected_signature =
                    request.presentation->projection_signature;
                if (reprojected_signature == projection) {
                    if (request.presentation->rich.flow_overflow_regions !=
                        0) {
                        return result(MessageTextCoordinatorError::overflow,
                                      passes);
                    }
                    if (!request.finalize(request.finalize_context,
                                          *request.theme)) {
                        return result(
                            MessageTextCoordinatorError::finalization,
                            passes);
                    }
                    return result(MessageTextCoordinatorError::none, passes,
                                  MessageSceneError::none,
                                  MessageTextPresentationError::none, true);
                }
            }
            continue;
        }
        if (request.presentation->rich.flow_overflow_regions != 0) {
            return result(MessageTextCoordinatorError::overflow, passes);
        }
        if (!request.finalize(request.finalize_context, *request.theme)) {
            return result(MessageTextCoordinatorError::finalization, passes);
        }
        return result(MessageTextCoordinatorError::none, passes, MessageSceneError::none,
                      MessageTextPresentationError::none, true);
    }
    return result(MessageTextCoordinatorError::non_convergent,
                  static_cast<u8>(kMaximumMessageTextConvergencePasses));
}

} // namespace maicord

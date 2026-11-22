#include "maicord/app/chat/messages/reaction_viewer_storage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/chat/messages/reaction_viewer_controller.hpp"

namespace maicord {

void reaction_viewer_storage_release(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                     ReactionViewerController* controller) noexcept {
    stable_storage_release(&storage->page_users);
    stable_storage_release(&storage->text);
    stable_storage_release(&storage->users);
    reset_value<ReactionViewerStorage>(storage);
    reset_value<ReactionViewerController>(controller);
    reset_value<ReactionViewer>(viewer);
}

bool reaction_viewer_storage_materialize(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                         ReactionViewerController* controller) noexcept {
    // Ready storage stays bound to the one viewer and controller; a failed materialize releases what it took.
    if (storage->ready)
        return true;
    if (!stable_storage_initialize(&storage->users, kReactionViewerUserCapacity,
                                   kReactionViewerUserCapacity, sizeof(ReactionViewerUser),
                                   alignof(ReactionViewerUser)) ||
        !stable_storage_initialize(&storage->text, kReactionViewerTextCapacity, kReactionViewerTextCapacity,
                                   sizeof(char), alignof(char)) ||
        !stable_storage_initialize(&storage->page_users, kReactionUsersMaximumPage,
                                   kReactionUsersMaximumPage, sizeof(ReactionUser),
                                   alignof(ReactionUser)) ||
        !reaction_viewer_initialize(
            viewer,
            Slice<ReactionViewerUser>{stable_storage_data<ReactionViewerUser>(storage->users),
                                      kReactionViewerUserCapacity},
            Slice<char>{stable_storage_data<char>(storage->text), kReactionViewerTextCapacity}) ||
        !reaction_viewer_controller_initialize(
            controller, viewer,
            Slice<ReactionUser>{stable_storage_data<ReactionUser>(storage->page_users),
                                kReactionUsersMaximumPage})) {
        reaction_viewer_storage_release(storage, viewer, controller);
        return false;
    }
    storage->ready = true;
    return true;
}

} // namespace maicord

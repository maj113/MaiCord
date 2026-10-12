#include "maicord/app/reaction_viewer_storage.hpp"

namespace maicord {

void reaction_viewer_storage_release(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                     ReactionViewerController* controller) noexcept {
    if (storage) {
        stable_storage_release(&storage->page_users);
        stable_storage_release(&storage->text);
        stable_storage_release(&storage->users);
        *storage = ReactionViewerStorage{};
    }
    if (controller) {
        *controller = ReactionViewerController{};
    }
    if (viewer) {
        *viewer = ReactionViewer{};
    }
}

bool reaction_viewer_storage_materialize(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                         ReactionViewerController* controller) noexcept {
    if (!storage || !viewer || !controller) {
        return false;
    }
    if (storage->ready) {
        return controller->initialized && controller->viewer == viewer &&
               viewer->users == stable_storage_data<ReactionViewerUser>(storage->users) &&
               viewer->text == stable_storage_data<char>(storage->text) &&
               controller->page_users == stable_storage_data<ReactionUser>(storage->page_users);
    }
    if (storage->users.arena.base || storage->text.arena.base || storage->page_users.arena.base) {
        reaction_viewer_storage_release(storage, viewer, controller);
    }
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

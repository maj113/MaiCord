#pragma once

#include "maicord/app/reaction_viewer_controller.hpp"
#include "maicord/core/stable_storage.hpp"

namespace maicord {

// Backing storage for the reaction modal is dormant until the modal is first
// opened. The renderer may still bind the zeroed viewer while it is inactive.
struct ReactionViewerStorage {
    StableStorage users;
    StableStorage text;
    StableStorage page_users;
    bool ready;
};

bool reaction_viewer_storage_materialize(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                         ReactionViewerController* controller) noexcept;
void reaction_viewer_storage_release(ReactionViewerStorage* storage, ReactionViewer* viewer,
                                     ReactionViewerController* controller) noexcept;

} // namespace maicord

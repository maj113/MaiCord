#include "application_friends_search.hpp"
namespace maicord::app_detail {
bool friends_search_documents_initialize(FriendsSearchDocuments* documents) noexcept {
    for (u32 i = 0; i < 4; ++i) {
        if (!message_composer_initialize(&documents->models[i], {documents->text[i], kFriendsSearchBytes})) {
            friends_search_documents_release(documents);
            return false;
        }
    }
    return true;
}
void friends_search_documents_release(FriendsSearchDocuments* documents) noexcept {
    for (auto& model : documents->models)
        message_composer_release(&model);
    *documents = {};
}
MessageComposer* friends_search_document(FriendsSearchDocuments* documents, UiFriendsTab tab) noexcept {
    return tab < UiFriendsTab::add_friend ? &documents->models[static_cast<u32>(tab)] : nullptr;
}
} // namespace maicord::app_detail

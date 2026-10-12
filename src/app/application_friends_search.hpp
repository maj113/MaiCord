#pragma once
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/friends_scene.hpp"

namespace maicord::app_detail {
inline constexpr u32 kFriendsSearchBytes = 128;
struct FriendsSearchDocuments {
    MessageComposer models[4];
    char text[4][kFriendsSearchBytes];
};
bool friends_search_documents_initialize(FriendsSearchDocuments*) noexcept;
void friends_search_documents_release(FriendsSearchDocuments*) noexcept;
MessageComposer* friends_search_document(FriendsSearchDocuments*, UiFriendsTab) noexcept;
}

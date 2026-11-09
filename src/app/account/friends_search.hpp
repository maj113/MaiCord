#pragma once
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/protocol/user/relationships.hpp"
#include "maicord/ui/account/friends_scene.hpp"

namespace maicord::app_detail {
inline constexpr u32 kFriendsSearchBytes = 128;
inline constexpr u32 kFriendsSearchDocuments = static_cast<u32>(UiFriendsTab::add_friend) + 1;
// One query per list tab, the Add Friend tab's username field and its request note.
struct FriendsSearchDocuments {
    MessageComposer models[kFriendsSearchDocuments];
    MessageComposer note;
    char text[kFriendsSearchDocuments][kFriendsSearchBytes];
    char note_text[kRelationshipNoteMaximumBytes];
};
bool friends_search_documents_initialize(FriendsSearchDocuments*) noexcept;
void friends_search_documents_release(FriendsSearchDocuments*) noexcept;
MessageComposer* friends_search_document(FriendsSearchDocuments*, UiFriendsTab) noexcept;
}

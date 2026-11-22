#include "../../../../src/app/chat/threads/forum_form.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"

namespace maicord {
void test_application_forum_form(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    const auto named_tag = utf8_literal("Questions");
    check(utf8_equal(forum_post_tag_label({}), utf8_literal("Unnamed tag")) &&
              utf8_equal(forum_post_tag_label(named_tag), named_tag),
          "forum_form_tag_label_policy_covers_empty_wire_name_and_nonempty_name");
    SecureEdit title{}, body{};
    check(secure_edit_initialize(&title, kApplicationForumTitleBytes, false,
                                 {kApplicationForumTitleScalars, false}) &&
              secure_edit_initialize(&body, kApplicationForumBodyBytes, false,
                                     {kApplicationForumBodyScalars, true}),
          "forum_form_native_edit_models_initialize_bounded_two_fields");
    if (!title.storage.data || !body.storage.data) {
        secure_edit_release(&title);
        secure_edit_release(&body);
        return;
    }
    char unicode[8000]{};
    const char emoji[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    for (u32 i = 0; i < 2000; ++i)
        copy_bytes(unicode + i * 4, emoji, 4);
    const auto* body_storage = body.storage.data;
    check(secure_edit_assign(&body, {unicode, sizeof(unicode)}) == SecureEditResult::changed &&
              body.storage.bytes == 8000,
          "forum_form_full8000byte_2000scalar_message_without_truncation");
    check(secure_edit_insert_utf8(&body, utf8_literal("x")) == SecureEditResult::capacity &&
              body.storage.bytes == 8000 && body.storage.data == body_storage,
          "forum_form_scalar_or_byte_overflow_atomic_retains_complete_message");
    check(secure_edit_assign(&title, {unicode, 400}) == SecureEditResult::changed &&
              title.storage.bytes == 400 &&
              secure_edit_insert_utf8(&title, utf8_literal("x")) == SecureEditResult::capacity,
          "forum_form_100unicode_scalar_title_exact400byte_boundary");
    check(secure_edit_assign(&body, utf8_literal("First\nSecond")) == SecureEditResult::changed &&
              secure_edit_enable_history(&body, 32768) &&
              secure_edit_insert_utf8(&body, utf8_literal("\nThird")) == SecureEditResult::changed &&
              secure_edit_undo(&body) == SecureEditResult::changed && body.storage.bytes == 12 &&
              secure_edit_redo(&body) == SecureEditResult::changed && body.storage.bytes == 18,
          "forum_form_multiline_message_preserves_lines_native_undo_redo");
    secure_edit_release(&title);
    secure_edit_release(&body);
    secure_clear_bytes(unicode, sizeof(unicode));
}
} // namespace maicord

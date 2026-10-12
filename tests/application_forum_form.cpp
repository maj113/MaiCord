#include "../src/app/application_forum_form.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_application_forum_form(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    const auto named_tag = utf8_literal("Questions");
    check(utf8_equal(forum_post_tag_label({}), utf8_literal("Unnamed tag")) &&
              forum_post_tag_label(named_tag).data == named_tag.data,
          "forum_form_tag_label_policy_covers_empty_wire_name_and_borrows_nonempty_name");
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
    const auto* title_storage = title.storage.data;
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
    secure_edit_assign(&title, utf8_literal("Title"));
    const auto old_title = title.revision;
    check(secure_edit_insert_utf8(&title, utf8_literal("\n")) == SecureEditResult::invalid_input &&
              title.revision == old_title && title.storage.bytes == 5,
          "forum_form_singleline_title_rejects_newline_atomically");
    check(secure_edit_assign(&body, utf8_literal("First\nSecond")) == SecureEditResult::changed &&
              secure_edit_enable_history(&body, 32768) &&
              secure_edit_insert_utf8(&body, utf8_literal("\nThird")) == SecureEditResult::changed &&
              secure_edit_undo(&body) == SecureEditResult::changed && body.storage.bytes == 12 &&
              secure_edit_redo(&body) == SecureEditResult::changed && body.storage.bytes == 18,
          "forum_form_multiline_message_preserves_lines_native_undo_redo");
    SecureEditUtf16Selection selection{};
    bool stable = true;
    for (u32 i = 0; i < 64; ++i)
        stable = secure_edit_utf16_selection(body, &selection) == SecureEditResult::unchanged &&
                 secure_edit_set_utf16_selection(&body, selection) == SecureEditResult::unchanged && stable;
    check(stable && body.storage.data == body_storage && title.storage.data == title_storage,
          "forum_form_64_selection_projections_keep_independent_edit_allocations");
    secure_edit_release(&title);
    secure_edit_release(&body);
    SecureZeroMemory(unicode, sizeof(unicode));
    check(!title.storage.data && !body.storage.data, "forum_form_edit_models_release_both_lazy_buffers");
}
} // namespace maicord

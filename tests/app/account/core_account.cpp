#include "../../support/test_main_shared.hpp"

#include "maicord/app/account/account_paths.hpp"
#include "maicord/app/account/account_host.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/app/account/account_profile_promotion.hpp"
#include "maicord/app/account/account_session_store.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/chat/composer/message_draft_persistence.hpp"
#include "maicord/app/chat/composer/message_draft_session.hpp"
#include "maicord/app/account/login/remote_auth_ticket_exchange.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/protocol/messages/history.hpp"
#include "maicord/ui/text/text.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/protocol/messages/poll.hpp"
#include "maicord/protocol/user/private_conversation.hpp"
#include "maicord/core/win32_exception.hpp"
#include "maicord/protocol/user/relationships.hpp"

namespace maicord {
namespace {

bool wide_text_equal(const wchar_t* left, const wchar_t* right) noexcept {
    if (!left || !right) {
        return left == right;
    }
    u32 index = 0;
    while (left[index] != 0 && right[index] != 0) {
        if (left[index] != right[index]) {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

bool account_profile_test_path(wchar_t* output, u32 capacity) noexcept {
    if (!output || capacity < 64) {
        return false;
    }
    const DWORD prefix = GetTempPathW(capacity, output);
    if (prefix == 0 || prefix >= capacity) {
        return false;
    }
    constexpr wchar_t name[] = L"MaiCord-account-profile-";
    u32 cursor = prefix;
    for (u32 index = 0; index + 1 < sizeof(name) / sizeof(name[0]); ++index) {
        if (cursor + 1 >= capacity) {
            return false;
        }
        output[cursor++] = name[index];
    }
    u32 process = GetCurrentProcessId();
    wchar_t digits[16]{};
    u32 digit_count = 0;
    do {
        digits[digit_count++] = static_cast<wchar_t>(L'0' + process % 10);
        process /= 10;
    } while (process != 0);
    if (cursor + digit_count + 5 >= capacity) {
        return false;
    }
    while (digit_count != 0) {
        output[cursor++] = digits[--digit_count];
    }
    output[cursor++] = L'.';
    output[cursor++] = L'b';
    output[cursor++] = L'i';
    output[cursor++] = L'n';
    output[cursor] = 0;
    return true;
}

} // namespace

void test_account_paths() noexcept {
    AccountPaths paths{};
    const AccountPathsResult built = account_paths_build_slot(L"C:\\Users\\Example\\AppData\\Local", 0, &paths);
    expect(
        built.ok() &&
            wide_text_equal(paths.application_directory, L"C:\\Users\\Example\\AppData\\Local\\MaiCord") &&
            wide_text_equal(paths.account_directory,
                            L"C:\\Users\\Example\\AppData\\Local\\MaiCord\\accounts") &&
            wide_text_equal(paths.profile,
                            L"C:\\Users\\Example\\AppData\\Local\\MaiCord\\accounts\\active.profile") &&
            wide_text_equal(paths.session,
                            L"C:\\Users\\Example\\AppData\\Local\\MaiCord\\accounts\\active.session") &&
            wide_text_equal(paths.drafts,
                            L"C:\\Users\\Example\\AppData\\Local\\MaiCord\\accounts\\active.drafts"),
        "account_paths_canonical");

    AccountPaths trailing{};
    expect(account_paths_build_slot(L"C:\\Local\\", 0, &trailing).ok() &&
               wide_text_equal(trailing.profile, L"C:\\Local\\MaiCord\\accounts\\active.profile"),
           "account_paths_trailing_separator");
    expect(account_paths_build_slot(nullptr, 0, &trailing).error == AccountPathsError::invalid_input &&
               account_paths_build_slot(L"", 0, &trailing).error == AccountPathsError::invalid_input &&
               account_paths_build_slot(L"C:\\Local", 0, nullptr).error == AccountPathsError::invalid_input,
           "account_paths_invalid_contract");

    wchar_t oversized[kAccountProfileMaximumPathCharacters]{};
    for (u32 index = 0; index < kAccountProfileMaximumPathCharacters - 1; ++index) {
        oversized[index] = L'x';
    }
    expect(account_paths_build_slot(oversized, 0, &trailing).error == AccountPathsError::capacity,
           "account_paths_capacity");
}

namespace {

bool write_message_draft_test_image(const wchar_t* path, Slice<u8> image) noexcept {
    if (!path || !image.data || image.count < 56) return false;
    byte_store_le64(image.data + 48, 0);
    byte_store_le64(image.data + 48, hash64_bytes(kHash64Seed, image.data, image.count));
    // Production preserves the hidden attribute on replacement. Open that exact
    // fixture instead of CREATE_ALWAYS with conflicting normal attributes.
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, image.data, image.count, &written, nullptr) &&
        written == image.count && SetEndOfFile(file) && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

void test_message_draft_session(const wchar_t* path, u64 profile_id) noexcept {
    // The caller has written the independently encoded v2 fixture above.
    MessageDraftRecord records[4]{}, verification_records[4]{};
    u8 text[256]{}, verification_text[256]{};
    MessageDraftStore drafts{}, verification{};
    MessageDraftSession session{};
    const bool ready = message_draft_store_initialize(&drafts, {records, 4}, {text, sizeof(text)}) &&
        message_draft_store_initialize(&verification, {verification_records, 4},
                                       {verification_text, sizeof(verification_text)}) &&
        message_draft_store_save(&drafts, {99, utf8_literal("prior account"), 0, 13, 990, false}).ok();
    const bool configured = ready && message_draft_session_configure(session, drafts, path, profile_id, 1'000);
    MessageDraftSnapshot snapshot{};
    expect(configured && session.profile_id == profile_id && session.cleanup_required &&
        session.persisted_revision == 12 && drafts.revision == 12 && drafts.count == 1 &&
        session.persist_at_ms == 1'500 &&
        !message_draft_store_snapshot(drafts, 99, &snapshot) &&
        message_draft_store_snapshot(drafts, 7, &snapshot) && snapshot.reply_message_id == 0 &&
        snapshot.mention_reply_author && snapshot.touched == 17 && snapshot.revision == 3,
        "message_draft_session_discarding_load_marks_cleanup_and_schedules_throttled_rewrite");
    message_draft_session_observe(session, drafts, 1'200);
    message_draft_session_persist(session, drafts, 1'499, false);
    const MessageDraftPersistenceResult before_deadline = message_drafts_load_file(path, profile_id, &verification);
    expect(before_deadline.ok() && before_deadline.discarded == 1 && session.cleanup_required &&
        session.persist_at_ms == 1'500,
        "message_draft_session_cleanup_keeps_first_deadline_and_file_until_due");
    message_draft_session_persist(session, drafts, 1'500, false);
    const bool queued = session.persist_in_flight;
    message_draft_session_persist(session, drafts, 1'500, true);
    const bool migration_committed = queued && !session.persist_in_flight;
    const MessageDraftPersistenceResult rewritten = message_drafts_load_file(path, profile_id, &verification);
    expect(migration_committed && rewritten.ok() && rewritten.discarded == 0 && rewritten.records == 1 &&
        rewritten.bytes == 114 && !session.cleanup_required &&
        session.persist_at_ms == 0 && session.persist_failures == 0 &&
        session.persisted_revision == 12 && drafts.revision == 12,
        "message_draft_session_due_cleanup_rewrites_and_marks_exact_revision_persisted");

    const bool dirty_saved = message_draft_store_save(
        &drafts, {7, utf8_literal("dirty reply"), 2, 11, 707, false}).ok();
    message_draft_session_observe(session, drafts, 2'000);
    const u32 dirty_revision = drafts.revision;
    u8 corrupt[56]{};
    const bool corrupt_written = write_message_draft_test_image(path, {corrupt, sizeof(corrupt)});
    const bool same_corrupt = message_draft_session_configure(session, drafts, path, profile_id, 2'100);
    expect(dirty_saved && corrupt_written && same_corrupt && drafts.revision == dirty_revision &&
        dirty_revision != 12 && session.persisted_revision == 12 && session.persist_at_ms == 2'500 &&
        !session.cleanup_required && drafts.count == 1 &&
        message_draft_store_snapshot(drafts, 7, &snapshot) && snapshot.reply_message_id == 707 &&
        !snapshot.mention_reply_author && snapshot.anchor == 2 && snapshot.caret == 11 &&
        utf8_equal(snapshot.text, utf8_literal("dirty reply")),
        "message_draft_session_same_profile_corrupt_reload_preserves_dirty_reply_and_persisted_revision");
    const bool same_deleted = DeleteFileW(path) != FALSE;
    const bool same_missing = message_draft_session_configure(session, drafts, path, profile_id, 2'200);
    expect(same_deleted && same_missing && drafts.revision == dirty_revision &&
        session.persisted_revision == 12 && session.persist_at_ms == 2'500 &&
        message_draft_store_snapshot(drafts, 7, &snapshot) && snapshot.reply_message_id == 707 &&
        !snapshot.mention_reply_author && snapshot.anchor == 2 && snapshot.caret == 11 &&
        utf8_equal(snapshot.text, utf8_literal("dirty reply")),
        "message_draft_session_same_profile_missing_reload_keeps_dirty_text_reply_and_deadline");
    message_draft_session_persist(session, drafts, 2'499, false);
    const MessageDraftPersistenceResult still_missing = message_drafts_load_file(path, profile_id, &verification);
    expect(still_missing.error == MessageDraftPersistenceError::open &&
        (still_missing.native_error == ERROR_FILE_NOT_FOUND || still_missing.native_error == ERROR_PATH_NOT_FOUND) &&
        session.persisted_revision == 12 && session.persist_at_ms == 2'500,
        "message_draft_session_failed_reload_does_not_skip_dirty_save_or_advance_deadline");
    message_draft_session_persist(session, drafts, 2'500, false);
    message_draft_session_persist(session, drafts, 2'500, true);
    const bool dirty_committed = !session.persist_in_flight;
    const MessageDraftPersistenceResult dirty_rewritten = message_drafts_load_file(path, profile_id, &verification);
    expect(dirty_committed && dirty_rewritten.ok() && dirty_rewritten.bytes == 115 &&
        session.persisted_revision == dirty_revision && session.persist_at_ms == 0 &&
        message_draft_store_snapshot(verification, 7, &snapshot) && snapshot.reply_message_id == 707 &&
        !snapshot.mention_reply_author && snapshot.anchor == 2 && snapshot.caret == 11 &&
        utf8_equal(snapshot.text, utf8_literal("dirty reply")),
        "message_draft_session_dirty_reply_survives_corrupt_missing_reload_and_is_eventually_persisted");

    const bool other_deleted = DeleteFileW(path) != FALSE;
    const bool other_missing = message_draft_session_configure(session, drafts, path, profile_id + 1, 3'000);
    bool text_erased = guard_intact(text, sizeof(text), 0);
    expect(other_deleted && other_missing && session.profile_id == profile_id + 1 &&
        session.persisted_revision == 0 && session.persist_at_ms == 0 &&
        !session.cleanup_required && drafts.count == 0 && drafts.revision == 0 && drafts.clock == 0 &&
        drafts.text_used == 0 && drafts.text_wasted == 0 && text_erased &&
        !message_draft_store_snapshot(drafts, 7, &snapshot),
        "message_draft_session_new_missing_profile_erases_previous_profile_text_and_reply");
    const bool other_ready = message_draft_store_save(
        &drafts, {7, utf8_literal("other account"), 1, 13, 717, false}).ok() &&
        message_drafts_save_file(path, profile_id + 1, drafts).ok() &&
        write_message_draft_test_image(path, {corrupt, sizeof(corrupt)});
    const bool other_corrupt = message_draft_session_configure(session, drafts, path, profile_id + 2, 3'100);
    text_erased = guard_intact(text, sizeof(text), 0);
    expect(other_ready && other_corrupt && session.profile_id == profile_id + 2 &&
        session.persisted_revision == 0 && session.persist_at_ms == 0 &&
        !session.cleanup_required && drafts.count == 0 && drafts.revision == 0 && drafts.clock == 0 &&
        drafts.text_used == 0 && text_erased && records[0].reply_message_id == 0 &&
        !message_draft_store_snapshot(drafts, 7, &snapshot),
        "message_draft_session_new_corrupt_profile_cannot_retain_previous_profile_reply_or_text");
}

void test_message_reply_draft_persistence(const wchar_t* path, u64 profile_id) noexcept {
    MessageDraftRecord records[4]{};
    u8 text[256]{};
    MessageDraftStore source{};
    const Utf8View unicode = utf8_literal("draft \xF0\x9F\x9A\x80");
    const bool ready = message_draft_store_initialize(&source, {records, 4}, {text, sizeof(text)}) &&
        message_draft_store_save(&source, {7, unicode, 6, unicode.bytes, 701, false}).ok() &&
        message_draft_store_save(&source, {8, utf8_literal("\t\xC2\xA0\n"), 0, 4, 801}).ok() &&
        message_draft_store_save(&source, {9, Utf8View{}, 0, 0, 901, false}).ok() &&
        message_draft_store_save(&source, {10, utf8_literal("ordinary"), 0, 8}).ok();
    const MessageDraftPersistenceResult saved = ready
        ? message_drafts_save_file(path, profile_id, source) : MessageDraftPersistenceResult{};
    MessageDraftRecord restored_records[4]{};
    u8 restored_text[256]{};
    MessageDraftStore restored{};
    const bool restored_ready = message_draft_store_initialize(
        &restored, {restored_records, 4}, {restored_text, sizeof(restored_text)});
    const MessageDraftPersistenceResult loaded = restored_ready
        ? message_drafts_load_file(path, profile_id, &restored) : MessageDraftPersistenceResult{};
    MessageDraftSnapshot first{}, blank{}, reply_only{}, ordinary{};
    expect(ready && saved.ok() && saved.bytes == 56 + 4 * 48 + 22 && loaded.ok() &&
        loaded.records == 4 && loaded.discarded == 0 &&
        message_draft_store_snapshot(restored, 7, &first) && first.reply_message_id == 701 &&
        !first.mention_reply_author && first.anchor == 6 && first.caret == unicode.bytes &&
        utf8_equal(first.text, unicode) && message_draft_store_snapshot(restored, 8, &blank) &&
        blank.reply_message_id == 801 && blank.mention_reply_author && blank.text.bytes == 4 &&
        message_draft_store_snapshot(restored, 9, &reply_only) && reply_only.text.bytes == 0 &&
        reply_only.reply_message_id == 901 && !reply_only.mention_reply_author &&
        message_draft_store_snapshot(restored, 10, &ordinary) && ordinary.reply_message_id == 0 &&
        ordinary.mention_reply_author,
        "message_reply_draft_v2_round_trip_preserves_text_reply_only_blank_reply_and_ping_policy");

    u8 original[512]{}, image[512]{};
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    bool image_ready = saved.ok() && saved.bytes <= sizeof(original) && file != INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD read = 0;
        image_ready = image_ready && ReadFile(file, original, saved.bytes, &read, nullptr) && read == saved.bytes;
        CloseHandle(file);
    }
    const MessageDraftStore preserved = restored;
    MessageDraftRecord preserved_records[4]{};
    u8 preserved_text[256]{};
    copy_bytes(preserved_records, restored_records, sizeof(preserved_records));
    copy_bytes(preserved_text, restored_text, sizeof(preserved_text));
    const MessageDraftPersistenceResult wrong_profile = message_drafts_load_file(path, profile_id + 1, &restored);
    expect(wrong_profile.error == MessageDraftPersistenceError::profile &&
        maicord::bytes_equal(&restored, &preserved, sizeof(restored)) &&
        maicord::bytes_equal(restored_records, preserved_records, sizeof(preserved_records)) &&
        maicord::bytes_equal(restored_text, preserved_text, sizeof(preserved_text)),
        "message_reply_draft_wrong_profile_keeps_all_reply_targets_policies_and_text_atomically");
    bool invalid_atomic = image_ready;
    for (u32 variant = 0; image_ready && variant < 8; ++variant) {
        copy_bytes(image, original, saved.bytes);
        if (variant == 0) byte_store_le32(image + 56 + 44, 1); // Reserved v2 word.
        if (variant == 1) byte_store_le32(image + 56 + 40, 2); // Unknown reply flag.
        if (variant == 2) byte_store_le32(image + 56 + 3 * 48 + 40, 1); // No-target ping suppression.
        if (variant == 3) byte_store_le64(image + 56 + 32, 0); // Suppression with removed target.
        if (variant == 4) {
            byte_store_le64(image + 56 + 2 * 48 + 32, 0); // Empty record without reply.
            byte_store_le32(image + 56 + 2 * 48 + 40, 0);
        }
        if (variant == 5) byte_store_le64(image + 56 + 2 * 48, 7); // Duplicate channel.
        if (variant == 6) byte_store_le32(image + 56 + 20, 7); // Selection inside UTF-8 scalar.
        if (variant == 7) byte_store_le32(image + 56 + 28, 0); // Invalid record revision.
        const bool written = write_message_draft_test_image(path, {image, saved.bytes});
        const MessageDraftPersistenceResult rejected = written
            ? message_drafts_load_file(path, profile_id, &restored) : MessageDraftPersistenceResult{};
        invalid_atomic = invalid_atomic && written && rejected.error == MessageDraftPersistenceError::format &&
            maicord::bytes_equal(&restored, &preserved, sizeof(restored)) &&
            maicord::bytes_equal(restored_records, preserved_records, sizeof(preserved_records)) &&
            maicord::bytes_equal(restored_text, preserved_text, sizeof(preserved_text));
    }
    expect(invalid_atomic,
        "message_reply_draft_v2_rejects_checksummed_reserved_flags_empty_duplicate_selection_revision_atomically");
    expect(image_ready && write_message_draft_test_image(path, {original, saved.bytes}),
        "message_reply_draft_restore_valid_v2_fixture");
    MessageDraftRecord small_records[3]{};
    u8 small_text[256]{};
    MessageDraftStore bounded{};
    const bool small_ready = message_draft_store_initialize(&bounded, {small_records, 3}, {small_text, sizeof(small_text)}) &&
        message_draft_store_save(&bounded, {99, Utf8View{}, 0, 0, 990, false}).ok();
    const MessageDraftPersistenceResult capacity = small_ready
        ? message_drafts_load_file(path, profile_id, &bounded) : MessageDraftPersistenceResult{};
    expect(small_ready && capacity.error == MessageDraftPersistenceError::capacity && bounded.count == 1 &&
        message_draft_store_snapshot(bounded, 99, &reply_only) && reply_only.reply_message_id == 990 &&
        !reply_only.mention_reply_author && reply_only.text.bytes == 0,
        "message_reply_draft_capacity_failure_keeps_existing_reply_only_draft");

    const MessageDraftStoreResult cleared = message_draft_store_clear_reply(&restored, 9, 901);
    const MessageDraftPersistenceResult deleted = message_drafts_save_file(path, profile_id, restored);
    const MessageDraftPersistenceResult deletion_loaded = message_drafts_load_file(path, profile_id, &source);
    expect(cleared.ok() && cleared.changed && deleted.ok() && deleted.records == 3 && deletion_loaded.ok() &&
        !message_draft_store_snapshot(source, 9, &reply_only) &&
        message_draft_store_snapshot(source, 7, &first) && first.reply_message_id == 701 &&
        !first.mention_reply_author,
        "message_reply_draft_successful_clear_is_durable_without_touching_other_channels");

    // Independently encoded v2 fixture: an ordinary draft and a blank ordinary
    // draft that load discards, which schedules a cleanup rewrite.
    constexpr u32 fixture_size = 56 + 2 * 48 + 14;
    clear_bytes(image, sizeof(image));
    constexpr u8 magic[8] = {'M', 'A', 'I', 'C', 'D', 'R', 'F', 'T'};
    copy_bytes(image, magic, sizeof(magic));
    byte_store_le32(image + 8, 2);
    byte_store_le32(image + 12, 56);
    byte_store_le32(image + 16, fixture_size);
    byte_store_le32(image + 20, 2);
    byte_store_le32(image + 24, 14);
    byte_store_le32(image + 28, 12);
    byte_store_le64(image + 32, profile_id);
    byte_store_le64(image + 40, 19);
    byte_store_le64(image + 56, 7);
    byte_store_le64(image + 56 + 8, 17);
    byte_store_le32(image + 56 + 16, unicode.bytes);
    byte_store_le32(image + 56 + 20, 6);
    byte_store_le32(image + 56 + 24, unicode.bytes);
    byte_store_le32(image + 56 + 28, 3);
    byte_store_le64(image + 104, 8);
    byte_store_le64(image + 104 + 8, 19);
    byte_store_le32(image + 104 + 16, 4);
    byte_store_le32(image + 104 + 24, 4);
    byte_store_le32(image + 104 + 28, 2);
    copy_bytes(image + 152, unicode.data, unicode.bytes);
    copy_bytes(image + 152 + unicode.bytes, "\t\xC2\xA0\n", 4);
    const bool fixture_written = write_message_draft_test_image(path, {image, fixture_size});
    const MessageDraftPersistenceResult cleaned = fixture_written
        ? message_drafts_load_file(path, profile_id, &restored) : MessageDraftPersistenceResult{};
    expect(fixture_written && cleaned.ok() && cleaned.records == 1 && cleaned.discarded == 1 &&
        restored.revision == 12 && restored.clock == 19 &&
        message_draft_store_snapshot(restored, 7, &first) && first.reply_message_id == 0 &&
        first.mention_reply_author && first.touched == 17 && first.revision == 3 && first.anchor == 6 &&
        first.caret == unicode.bytes && utf8_equal(first.text, unicode),
        "message_reply_draft_load_preserves_metadata_and_discards_ordinary_blank");
    // Older versions are not migrated: a v1 header is rejected atomically and
    // the session treats the file like an absent store.
    byte_store_le32(image + 8, 1);
    const bool stale_written = fixture_written && write_message_draft_test_image(path, {image, fixture_size});
    const MessageDraftPersistenceResult stale = stale_written
        ? message_drafts_load_file(path, profile_id, &restored) : MessageDraftPersistenceResult{};
    expect(stale_written && stale.error == MessageDraftPersistenceError::version && restored.revision == 12 &&
        message_draft_store_snapshot(restored, 7, &first) && utf8_equal(first.text, unicode),
        "message_reply_draft_v1_file_is_rejected_without_migration");
    byte_store_le32(image + 8, 2);
    if (fixture_written && write_message_draft_test_image(path, {image, fixture_size}))
        test_message_draft_session(path, profile_id);
}

} // namespace

void test_message_draft_persistence() noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    expect(account_profile_test_path(path, kAccountProfileMaximumPathCharacters),
           "message_draft_persistence_test_path");
    if (path[0] == 0) {
        return;
    }
    DeleteFileW(path);

    MessageDraftRecord source_records[4]{};
    u8 source_text[256]{};
    MessageDraftStore source{};
    const Utf8View unicode = utf8_literal("draft \xF0\x9F\x9A\x80");
    const bool source_ready =
        message_draft_store_initialize(&source, Slice<MessageDraftRecord>{source_records, 4},
                                       Slice<u8>{source_text, sizeof(source_text)}) &&
        message_draft_store_save(&source, MessageDraftInput{7, unicode, 6, unicode.bytes}).ok() &&
        message_draft_store_save(&source, MessageDraftInput{8, utf8_literal("\t\xC2\xA0\n"), 0, 4}).ok() &&
        message_draft_store_save(&source, MessageDraftInput{9, utf8_literal("second"), 2, 5}).ok();
    const u64 first_touched = source.records[0].touched;
    const u32 first_revision = source.records[0].revision;
    constexpr u64 profile_id = 0x1122334455667788ull;
    const MessageDraftPersistenceResult saved =
        source_ready ? message_drafts_save_file(path, profile_id, source) : MessageDraftPersistenceResult{};

    MessageDraftRecord restored_records[4]{};
    u8 restored_text[256]{};
    MessageDraftStore restored{};
    const bool restored_ready =
        message_draft_store_initialize(&restored, Slice<MessageDraftRecord>{restored_records, 4},
                                       Slice<u8>{restored_text, sizeof(restored_text)}) &&
        message_draft_store_save(&restored, MessageDraftInput{99, utf8_literal("sentinel"), 0, 8}).ok();
    const MessageDraftPersistenceResult wrong_profile =
        restored_ready ? message_drafts_load_file(path, profile_id + 1, &restored)
                       : MessageDraftPersistenceResult{};
    MessageDraftSnapshot sentinel{};
    const bool profile_rejection_atomic = wrong_profile.error == MessageDraftPersistenceError::profile &&
                                          message_draft_store_snapshot(restored, 99, &sentinel) &&
                                          utf8_equal(sentinel.text, utf8_literal("sentinel"));

    const MessageDraftPersistenceResult loaded = restored_ready
                                                     ? message_drafts_load_file(path, profile_id, &restored)
                                                     : MessageDraftPersistenceResult{};
    MessageDraftSnapshot first{};
    MessageDraftSnapshot second{};
    const MessageDraftStoreResult first_found = message_draft_store_find(restored, 7);
    expect(source_ready && saved.ok() && saved.records == 3 &&
               saved.bytes <= kMessageDraftPersistenceMaximumFileBytes && profile_rejection_atomic &&
               loaded.ok() && loaded.records == 2 && loaded.discarded == 1 && restored.count == 2 &&
               message_draft_store_snapshot(restored, 7, &first) &&
               message_draft_store_snapshot(restored, 9, &second) &&
               !message_draft_store_snapshot(restored, 8, &sentinel) &&
               utf8_equal(first.text, unicode) && first.anchor == 6 && first.caret == unicode.bytes &&
               utf8_equal(second.text, utf8_literal("second")) && first_found.ok() &&
               restored.records[first_found.index].touched == first_touched &&
               restored.records[first_found.index].revision == first_revision &&
               restored.clock == source.clock && restored.revision == source.revision,
           "message_draft_persistence_profile_bound_round_trip_and_blank_cleanup");

    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool tampered = file != INVALID_HANDLE_VALUE;
    if (tampered) {
        LARGE_INTEGER end{};
        end.QuadPart = -1;
        u8 value = 0;
        DWORD transferred = 0;
        tampered = SetFilePointerEx(file, end, nullptr, FILE_END) &&
                   ReadFile(file, &value, 1, &transferred, nullptr) && transferred == 1;
        value ^= 0x40u;
        end.QuadPart = -1;
        transferred = 0;
        tampered = tampered && SetFilePointerEx(file, end, nullptr, FILE_END) &&
                   WriteFile(file, &value, 1, &transferred, nullptr) && transferred == 1 &&
                   FlushFileBuffers(file);
        CloseHandle(file);
    }
    const u32 unchanged_revision = restored.revision;
    const MessageDraftPersistenceResult corrupted =
        tampered ? message_drafts_load_file(path, profile_id, &restored) : MessageDraftPersistenceResult{};
    expect(tampered && corrupted.error == MessageDraftPersistenceError::format &&
               restored.revision == unchanged_revision && message_draft_store_snapshot(restored, 7, &first),
           "message_draft_persistence_rejects_tamper_atomically");

    expect(message_drafts_save_file(path, profile_id, restored).ok(),
           "message_draft_persistence_restore_fixture");
    file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool version_written = file != INVALID_HANDLE_VALUE;
    if (version_written) {
        LARGE_INTEGER offset{};
        offset.QuadPart = 8;
        const u8 version = kMessageDraftPersistenceVersion + 1;
        DWORD written = 0;
        version_written = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                          WriteFile(file, &version, 1, &written, nullptr) && written == 1 &&
                          FlushFileBuffers(file);
        CloseHandle(file);
    }
    const MessageDraftPersistenceResult future = version_written
                                                     ? message_drafts_load_file(path, profile_id, &restored)
                                                     : MessageDraftPersistenceResult{};
    expect(version_written && future.error == MessageDraftPersistenceError::version,
           "message_draft_persistence_rejects_future_version");

    message_draft_store_delete(&restored, 7);
    message_draft_store_delete(&restored, 9);
    const MessageDraftPersistenceResult emptied = message_drafts_save_file(path, profile_id, restored);
    MessageDraftRecord empty_records[2]{};
    u8 empty_text[32]{};
    MessageDraftStore empty{};
    const bool empty_ready = message_draft_store_initialize(
        &empty, Slice<MessageDraftRecord>{empty_records, 2}, Slice<u8>{empty_text, sizeof(empty_text)});
    const MessageDraftPersistenceResult empty_loaded =
        empty_ready ? message_drafts_load_file(path, profile_id, &empty) : MessageDraftPersistenceResult{};
    expect(emptied.ok() && emptied.records == 0 && empty_loaded.ok() && empty.count == 0 &&
               empty.text_used == 0,
           "message_draft_persistence_atomic_shrinking_overwrite");
    test_message_reply_draft_persistence(path, profile_id);
    DeleteFileW(path);
}

void test_account_host_contract() noexcept {
    AccountHost host{};
    const AccountHostResult loaded = account_host_load(&host);
    const HMODULE first_module = host.module;
    const AccountHostResult reused = account_host_load(&host);
    const bool reused_same_module = host.module == first_module;
    AccountHost unattached{};
    const bool attach_refuses_missing =
        account_host_attach(&unattached, nullptr).error == AccountHostError::invalid_input && !unattached.module;
    HANDLE handles[kAccountHostNotificationHandleCapacity] = {
        reinterpret_cast<HANDLE>(1), reinterpret_cast<HANDLE>(2), reinterpret_cast<HANDLE>(3)};
    const u32 empty_handles =
        account_host_notification_handles(&host, handles, kAccountHostNotificationHandleCapacity);

    AccountModuleStatus observed{};
    observed.coordinator_phase = 4;
    observed.gateway_ready = 1;
    observed.session_saves = 2;
    observed.session_save_failures = 3;
    observed.next_wake_at_ms = 150;
    const u32 first_observations = account_host_observe(&host, observed);
    const u32 repeated_observations = account_host_observe(&host, observed);
    host.instance = reinterpret_cast<void*>(1);
    const AccountHostResult active_release = account_host_release(&host);
    const u32 wait = account_host_wait_timeout(&host, 100);
    const bool due = account_host_wake_due(&host, 150);
    host.instance = nullptr;

    const AccountModuleStartResult invalid_start = account_host_start(&host, nullptr, nullptr, 1, 1);
    const AccountLoginStatus invalid_login = account_host_login_start(&host, nullptr);
    u32 pump_observations = ~0u;
    const AccountModuleStatus invalid_pump = account_host_pump(&host, 1, 1, &pump_observations);
    const AccountLoginStatus invalid_login_pump = account_host_login_pump(&host);
    const AccountLoginStatus invalid_login_stop = account_host_login_stop(&host, 1'000);
    const AccountHostStopResult stopped = account_host_stop(&host, 1'000, 1, 1);

    expect(attach_refuses_missing &&
               loaded.ok() && first_module == GetModuleHandleW(nullptr) && reused.ok() &&
               reused_same_module && host.module == nullptr && empty_handles == 0 &&
               handles[0] == reinterpret_cast<HANDLE>(1) &&
               first_observations == (account_host_observation_phase | account_host_observation_ready |
                                      account_host_observation_session_saves |
                                      account_host_observation_session_save_failures) &&
               repeated_observations == 0 && !active_release.ok() &&
               active_release.error == AccountHostError::active && wait == 50 && due &&
               invalid_start.error == AccountModuleError::invalid_input &&
               invalid_login.error == AccountLoginError::invalid_input &&
               invalid_pump.error == AccountModuleError::invalid_input && pump_observations == 0 &&
               invalid_login_pump.error == AccountLoginError::invalid_input &&
               invalid_login_stop.error == AccountLoginError::invalid_input && stopped.ok(),
           "account_host_bounded_module_lifecycle_contract");
    account_host_release(&host);
}

void test_account_profile() noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    expect(account_profile_test_path(path, kAccountProfileMaximumPathCharacters),
           "account_profile_test_path");
    if (path[0] == 0) {
        return;
    }
    DeleteFileW(path);

    SecureBuffer token{};
    const u8 token_bytes[] = {'u', 's', 'e', 'r', '.', 's', 'e', 's', 's', 'i', 'o', 'n'};
    expect(secure_buffer_initialize(&token, 64).ok() &&
               secure_buffer_assign(&token, ConstSlice<u8>{token_bytes, sizeof(token_bytes)}).ok(),
           "account_profile_user_token_fixture");
    if (!token.data) {
        return;
    }

    const AccountProfileMetadata metadata{0x1122334455667788ull};
    const AccountProfileResult saved = account_profile_save(path, metadata, token);
    AccountProfile loaded{};
    const AccountProfileResult restored =
        saved.ok() ? account_profile_load(path, &loaded) : AccountProfileResult{};
    bool equal = restored.ok() && loaded.metadata.profile_id == metadata.profile_id &&
                 loaded.credential.locked && loaded.credential.bytes == sizeof(token_bytes);
    for (u32 index = 0; equal && index < sizeof(token_bytes); ++index) {
        equal = loaded.credential.data[index] == token_bytes[index];
    }
    expect(saved.ok() && saved.bytes >= 48 && saved.bytes <= kAccountProfileMaximumFileBytes && equal,
           "account_profile_user_dpapi_round_trip");
    account_profile_release(&loaded);

    wchar_t failed_path[kAccountProfileMaximumPathCharacters]{};
    u32 failed_path_characters = 0;
    while (failed_path_characters < kAccountProfileMaximumPathCharacters - 7 &&
           path[failed_path_characters] != 0) {
        failed_path[failed_path_characters] = path[failed_path_characters];
        ++failed_path_characters;
    }
    constexpr wchar_t child[] = L"\\child";
    for (u32 index = 0; index < static_cast<u32>(sizeof(child) / sizeof(child[0])); ++index) {
        failed_path[failed_path_characters + index] = child[index];
    }
    AccountProfile promotion_source{};
    const SecureBufferResult promotion_initialized =
        secure_buffer_initialize(&promotion_source.credential, 64);
    const SecureBufferResult promotion_assigned =
        promotion_initialized.ok() ? secure_buffer_assign(&promotion_source.credential,
                                                          ConstSlice<u8>{token_bytes, sizeof(token_bytes)})
                                   : promotion_initialized;
    promotion_source.metadata = AccountProfileMetadata{0x8877665544332211ull};
    AccountProfilePromotion promotion{};
    const AccountProfilePromotionResult staged =
        promotion_assigned.ok() ? account_profile_promotion_stage(&promotion, &promotion_source)
                                : AccountProfilePromotionResult{};
    const AccountProfilePromotionResult failed_commit =
        staged.ok() ? account_profile_promotion_commit(&promotion, failed_path)
                    : AccountProfilePromotionResult{};
    expect(promotion_initialized.ok() && promotion_assigned.ok() && staged.ok() &&
               staged.phase == AccountProfilePromotionPhase::staged && !promotion_source.credential.data &&
               promotion_source.metadata.profile_id == 0 &&
               failed_commit.error == AccountProfilePromotionError::profile &&
               failed_commit.phase == AccountProfilePromotionPhase::staged &&
               account_profile_promotion_has_credential(promotion),
           "account_profile_promotion_failed_commit_retains_one_shot_credential");
    const AccountProfilePromotionResult retried = account_profile_promotion_commit(&promotion, path);
    AccountProfile promoted{};
    const AccountProfileResult promoted_loaded =
        retried.ok() ? account_profile_load(path, &promoted) : AccountProfileResult{};
    bool promoted_equal = promoted_loaded.ok() && promoted.metadata.profile_id == 0x8877665544332211ull &&
                          promoted.credential.bytes == sizeof(token_bytes);
    for (u32 index = 0; promoted_equal && index < sizeof(token_bytes); ++index) {
        promoted_equal = promoted.credential.data[index] == token_bytes[index];
    }
    expect(retried.ok() && retried.phase == AccountProfilePromotionPhase::committed &&
               !account_profile_promotion_has_credential(promotion) && promoted_equal &&
               account_profile_promotion_commit(&promotion, path).error ==
                   AccountProfilePromotionError::invalid_input,
           "account_profile_promotion_retry_commits_once_and_erases_owner");
    account_profile_release(&promoted);
    account_profile_release(&promotion_source);
    account_profile_promotion_release(&promotion);
    expect(promotion.state.phase == AccountProfilePromotionPhase::empty &&
               !promotion.profile.credential.data,
           "account_profile_promotion_release_is_terminal_and_empty");

    expect(account_profile_save(path, AccountProfileMetadata{0}, token).error ==
                   AccountProfileError::invalid_input &&
               token.data && token.bytes == sizeof(token_bytes),
           "account_profile_invalid_metadata_preserves_secret");

    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool tampered = file != INVALID_HANDLE_VALUE;
    if (tampered) {
        LARGE_INTEGER offset{};
        offset.QuadPart = 32;
        u8 value = 0;
        DWORD transferred = 0;
        tampered = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                   ReadFile(file, &value, 1, &transferred, nullptr) && transferred == 1;
        value ^= 1;
        transferred = 0;
        tampered = tampered && SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                   WriteFile(file, &value, 1, &transferred, nullptr) && transferred == 1 &&
                   FlushFileBuffers(file);
        CloseHandle(file);
    }
    AccountProfile rejected{};
    const AccountProfileResult rejected_metadata =
        tampered ? account_profile_load(path, &rejected) : AccountProfileResult{};
    expect(tampered && rejected_metadata.error == AccountProfileError::unprotect &&
               !rejected.credential.data,
           "account_profile_reject_bound_profile_tamper");

    expect(account_profile_save(path, metadata, token).ok(), "account_profile_rewrite_after_tamper");
    file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool version_written = file != INVALID_HANDLE_VALUE;
    if (version_written) {
        LARGE_INTEGER offset{};
        offset.QuadPart = 8;
        const u8 version = kAccountProfileVersion + 1;
        DWORD written = 0;
        version_written = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                          WriteFile(file, &version, 1, &written, nullptr) && written == 1 &&
                          FlushFileBuffers(file);
        CloseHandle(file);
    }
    const AccountProfileResult rejected_version =
        version_written ? account_profile_load(path, &rejected) : AccountProfileResult{};
    expect(version_written && rejected_version.error == AccountProfileError::version,
           "account_profile_reject_future_version");

    account_profile_release(&rejected);
    secure_buffer_release(&token);
    DeleteFileW(path);
}
void test_account_session_store() noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    expect(account_profile_test_path(path, kAccountProfileMaximumPathCharacters),
           "account_user_session_store_test_path");
    if (path[0] == 0) {
        return;
    }
    DeleteFileW(path);

    GatewayUserSessionSnapshot snapshot{};
    snapshot.profile_id = 0x8877665544332211ull;
    snapshot.self_user_id = 0x123456789abcdef0ull;
    constexpr char session_id[] = "session-state";
    constexpr char resume_url[] = "wss://gateway.discord.gg";
    copy_bytes(snapshot.resume.session_id, session_id, sizeof(session_id) - 1);
    copy_bytes(snapshot.resume.resume_url, resume_url, sizeof(resume_url) - 1);
    snapshot.resume.session_id_bytes = sizeof(session_id) - 1;
    snapshot.resume.resume_url_bytes = sizeof(resume_url) - 1;
    snapshot.resume.sequence = 77;
    snapshot.resume.valid = true;
    snapshot.has_resume = true;

    const AccountSessionStoreResult saved = account_user_session_store_save(path, snapshot);
    GatewayUserSessionSnapshot restored{};
    const AccountSessionStoreResult loaded =
        saved.ok() ? account_user_session_store_load(path, snapshot.profile_id, &restored)
                   : AccountSessionStoreResult{};
    expect(saved.ok() && saved.bytes >= 40 && saved.bytes <= kAccountSessionStoreMaximumFileBytes &&
               loaded.ok() && restored.has_resume && restored.profile_id == snapshot.profile_id &&
               restored.self_user_id == snapshot.self_user_id &&
               restored.resume.sequence == 77 &&
               restored.resume.session_id_bytes == sizeof(session_id) - 1 &&
               restored.resume.resume_url_bytes == sizeof(resume_url) - 1 &&
               restored.resume.session_id[0] == 's' && restored.resume.resume_url[3] == ':',
           "account_user_session_store_resume_round_trip");

    GatewayUserSessionSnapshot unchanged = restored;
    const AccountSessionStoreResult wrong_profile =
        account_user_session_store_load(path, snapshot.profile_id + 1, &unchanged);
    expect(wrong_profile.error == AccountSessionStoreError::format &&
               unchanged.profile_id == restored.profile_id &&
               unchanged.resume.sequence == restored.resume.sequence,
           "account_user_session_store_profile_binding");

    GatewayUserSessionSnapshot empty{};
    empty.profile_id = snapshot.profile_id;
    expect(account_user_session_store_save(path, empty).ok(),
           "account_user_session_store_empty_resume_save");
    GatewayUserSessionSnapshot empty_loaded{};
    const AccountSessionStoreResult empty_result =
        account_user_session_store_load(path, empty.profile_id, &empty_loaded);
    expect(empty_result.ok() && !empty_loaded.has_resume && !empty_loaded.resume.valid &&
               empty_loaded.profile_id == empty.profile_id,
           "account_user_session_store_empty_resume_round_trip");

    expect(account_user_session_store_save(path, snapshot).ok(),
           "account_user_session_store_restore_resume_fixture");
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool tampered = file != INVALID_HANDLE_VALUE;
    if (tampered) {
        LARGE_INTEGER offset{};
        offset.QuadPart = 32;
        u8 flags = 0;
        DWORD transferred = 0;
        tampered = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                   ReadFile(file, &flags, 1, &transferred, nullptr) && transferred == 1;
        flags ^= 1;
        transferred = 0;
        tampered = tampered && SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                   WriteFile(file, &flags, 1, &transferred, nullptr) && transferred == 1 &&
                   FlushFileBuffers(file);
        CloseHandle(file);
    }
    GatewayUserSessionSnapshot rejected{};
    const AccountSessionStoreResult rejected_tamper =
        tampered ? account_user_session_store_load(path, snapshot.profile_id, &rejected)
                 : AccountSessionStoreResult{};
    expect(tampered && rejected_tamper.error == AccountSessionStoreError::unprotect &&
               rejected.profile_id == 0,
           "account_user_session_store_reject_bound_header_tamper");
    DeleteFileW(path);
}
// A rejected dispatch is queued as a pointer-free marker: the recovery kind of
// its family, and for the rest the event name (never payload text).
void test_gateway_route_failure_marker() noexcept {
    u32 structurals[256]{};
    const auto marker = [&](Utf8View wire, GatewayClientEvent* event) noexcept {
        GatewayEnvelope envelope{};
        if (!gateway_decode_envelope(wire, {structurals, 256}, 16, &envelope).ok())
            return false;
        GatewayClientDispatchRouteResult routed{};
        routed.error = GatewayClientDispatchRouteError::decode;
        routed.event_error = GatewayEventDecodeError::missing_required_field;
        gateway_client_route_failure_event(envelope, routed, 77, event);
        return true;
    };
    GatewayClientEvent unknown{};
    const bool named =
        marker(utf8_literal(R"({"op":0,"s":7,"t":"THREAD_LIST_SYNC","d":{"guild_id":"1","secret":"x"}})"), &unknown);
    expect(named && unknown.kind == GatewayClientEventKind::dispatch &&
               unknown.dispatch_kind == GatewayClientDispatchKind::unknown && unknown.sequence == 7 &&
               unknown.has_sequence && unknown.timestamp_ms == 77 &&
               unknown.failure_stage == GatewayClientFailureStage::dispatch_route &&
               unknown.event_decode_error == GatewayEventDecodeError::missing_required_field &&
               unknown.event_name_bytes == 16 &&
               maicord::bytes_equal(unknown.event_name, "THREAD_LIST_SYNC", 17) && !unknown.dispatch_payload_bytes,
           "gateway_route_failure_marker_names_skipped_event");
    GatewayClientEvent votes{};
    expect(marker(utf8_literal(R"({"op":0,"s":8,"t":"MESSAGE_POLL_VOTE_ADD_MANY","d":{}})"), &votes) &&
               votes.dispatch_kind == GatewayClientDispatchKind::message_catchup_required &&
               !votes.event_name_bytes,
           "gateway_route_failure_marker_poll_vote_batch_requests_message_catchup");
    GatewayClientEvent supplemental{};
    expect(marker(utf8_literal(R"({"op":0,"s":2,"t":"READY_SUPPLEMENTAL","d":{}})"), &supplemental) &&
               supplemental.dispatch_kind == GatewayClientDispatchKind::private_state_incomplete,
           "gateway_route_failure_marker_supplemental_marks_private_state_incomplete");
}
void test_gateway_client_contract() noexcept {
    const GatewayUserClientConfiguration configuration{
        GatewayUserProperties{utf8_literal("Windows"), utf8_literal("Discord Client"),
                              utf8_literal("stable"), utf8_literal("1.0.9256"), utf8_literal("10.0.19045"),
                              utf8_literal("x64"), utf8_literal("x64"), utf8_literal("en-US"),
                              utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
                              utf8_literal("Agent/1.0 (Windows; native)"), utf8_literal("42.9.0"),
                              utf8_literal("19045"), 603738},
        0, GatewayUserStatus::online, false, false};
    GatewayUserClientConfiguration invalid_configuration = configuration;
    invalid_configuration.properties.client_launch_id = utf8_literal("not-a-launch-id");

    GatewayEndpoint endpoint{};
    expect(gateway_user_client_configuration_validate(configuration) &&
               !gateway_user_client_configuration_validate(invalid_configuration) &&
               gateway_endpoint_parse_with_policy(utf8_literal("wss://127.0.0.1:1"),
                                                  GatewayEndpointPolicy::user_v9_etf_zstd_stream,
                                                  &endpoint) == GatewayEndpointError::none,
           "gateway_client_user_configuration_and_runtime_contract");

    GatewayClient client{};
    AccountProfile profile{};
    profile.metadata = AccountProfileMetadata{0x1020304050607080ull};
    expect(secure_buffer_initialize(&profile.credential, 64).ok(), "gateway_client_user_token_initialize");
    if (!profile.credential.data) {
        return;
    }

    const u8 invalid_token[] = {0xFF};
    secure_buffer_assign(&profile.credential, ConstSlice<u8>{invalid_token, sizeof(invalid_token)});
    const GatewayClientResult invalid_token_result =
        gateway_client_start_user(&client, &profile, &configuration, &endpoint, nullptr);
    expect(invalid_token_result.error == GatewayClientError::invalid_input && profile.credential.data &&
               !client.runtime && !client.worker.thread,
           "gateway_client_reject_invalid_user_token_without_transfer");

    const u8 valid_token[] = {'t', 'e', 's', 't'};
    secure_buffer_assign(&profile.credential, ConstSlice<u8>{valid_token, sizeof(valid_token)});
    const GatewayClientResult invalid_configuration_result =
        gateway_client_start_user(&client, &profile, &invalid_configuration, &endpoint, nullptr);
    expect(invalid_configuration_result.error == GatewayClientError::invalid_input &&
               profile.credential.data && !client.runtime,
           "gateway_client_reject_user_configuration_without_transfer");

    GatewayUserSessionSnapshot mismatched_session{};
    mismatched_session.profile_id = profile.metadata.profile_id + 1;
    const GatewayClientResult mismatched_session_result =
        gateway_client_start_user(&client, &profile, &configuration, &endpoint, &mismatched_session);
    expect(mismatched_session_result.error == GatewayClientError::invalid_input &&
               profile.credential.data && !client.runtime,
           "gateway_client_reject_cross_profile_session_without_transfer");

    AccountProfile expanded_profile{};
    expanded_profile.metadata = AccountProfileMetadata{9};
    u8 expanded_token[kGatewayUserMaximumTokenBytes]{};
    guard_fill(expanded_token, sizeof(expanded_token), '"');
    const bool expanded_ready =
        secure_buffer_initialize(&expanded_profile.credential, kGatewayUserMaximumTokenBytes).ok() &&
        secure_buffer_assign(&expanded_profile.credential,
                             ConstSlice<u8>{expanded_token, static_cast<u32>(sizeof(expanded_token))})
            .ok();
    const GatewayClientResult expanded_result =
        expanded_ready
            ? gateway_client_start_user(&client, &expanded_profile, &configuration, &endpoint, nullptr)
            : GatewayClientResult{};
    expect(expanded_ready && expanded_result.error == GatewayClientError::invalid_input &&
               expanded_profile.credential.data &&
               expanded_profile.credential.bytes == sizeof(expanded_token) && !client.runtime,
           "gateway_client_exact_command_preflight_without_transfer");
    account_profile_release(&expanded_profile);
    SecureZeroMemory(expanded_token, sizeof(expanded_token));

    const GatewayClientResult started =
        gateway_client_start_user(&client, &profile, &configuration, &endpoint, nullptr);
    const bool owner_started = client.worker.thread && client.runtime && !profile.credential.data;
    GatewayUserSessionSnapshot snapshot{};
    const bool snapshot_ok = owner_started && gateway_client_snapshot_user_session(&client, &snapshot) &&
                             snapshot.profile_id == 0x1020304050607080ull && !snapshot.has_resume;
    AccountProfile returned{};
    const GatewayClientResult stopped =
        owner_started ? gateway_client_stop_return_user_profile(&client, 5'000, &returned)
                      : GatewayClientResult{};
    const ConstSlice<u8> returned_token = secure_buffer_view(returned.credential);
    expect(
        owner_started && snapshot_ok && !client.worker.thread && !client.runtime &&
            returned.metadata.profile_id == 0x1020304050607080ull &&
            returned_token.count == sizeof(valid_token) &&
            bytes_equal(returned_token, ConstSlice<u8>{valid_token, sizeof(valid_token)}) &&
            started.error != GatewayClientError::invalid_input &&
            (stopped.phase == GatewayClientPhase::stopped || stopped.phase == GatewayClientPhase::failed),
        "gateway_client_user_profile_handoff");

    account_profile_release(&returned);
    account_profile_release(&profile);
    expect(gateway_client_pop_event_payload(nullptr, nullptr, Slice<u8>{nullptr, 0}).error ==
                   GatewayClientPopError::invalid_input &&
               gateway_client_notification_handle(nullptr) == nullptr &&
               !gateway_client_snapshot_user_session(nullptr, nullptr) &&
               gateway_client_stop_return_user_profile(nullptr, 1, &profile).error ==
                   GatewayClientError::invalid_input,
           "gateway_client_null_observer_contract");
}

} // namespace maicord

#include "maicord/app/message_attachment_drafts.hpp"
namespace maicord {
void test_attachment_drafts(void (*check)(bool, const char*) noexcept) noexcept {
    MessageAttachmentDraftStore store{};
    if (!message_attachment_drafts_initialize(&store)) { check(false, "attachment drafts reserve bounded storage"); return; }
    check(!store.committed_bytes && message_attachment_drafts_reserved_bytes() < 5 * 1024 * 1024,
        "empty attachment drafts reserve address space without committing path payloads");
    wchar_t first_path[] = L"C:\\files\\first.txt";
    const wchar_t second_path[] = L"C:\\files\\second.txt";
    MessageAttachmentFile files[]{ {{first_path, 18}, {{5, 7, 9, 0}, 1024}}, {{second_path, 19}, {{6, 8, 9, 0}, 2048}} };
    u64 ids[10]{};
    const auto added = message_attachment_drafts_add(&store, 7, {files, 2}, {ids, 10});
    check(added == MessageAttachmentDraftError::none && store.count == 2 && store.committed_bytes == 8192 &&
        message_attachment_drafts_channel(&store, 7, {}) == 2 && !message_attachment_drafts_channel(&store, 8, {}),
        "selected files are staged atomically in their original channel");
    first_path[3] = L'X';
    const auto* first = message_attachment_drafts_get(&store, ids[0]);
    check(first && first->source.bytes == 1024 && first->source.identity.index == 5 &&
        message_attachment_drafts_path(&store, ids[0]).data[3] == L'f' &&
        message_attachment_drafts_filename(&store, ids[0]).bytes == 9,
        "staged path, filename and source metadata are owned independently of picker results");
    MessageAttachmentFile bad[]{files[0], files[1]}; bad[1].snapshot.bytes = 0;
    u64 rejected[2]{};
    check(message_attachment_drafts_add(&store, 8, {bad, 2}, {rejected, 2}) == MessageAttachmentDraftError::empty_file && store.count == 2,
        "invalid member prevents partial batch staging");
    UploadedAttachment reference{};
    check(!message_attachment_drafts_reference(&store, ids[0], 0, &reference) &&
        !message_attachment_drafts_mark_queued(&store, {ids, 2}, 100),
        "unuploaded attachments cannot enter a message");
    check(message_attachment_drafts_describe(&store, ids[0], utf8_literal("Alt text"), true) &&
        message_attachment_drafts_uploaded(&store, ids[0], utf8_literal("uploads/first.txt")) &&
        message_attachment_drafts_reference(&store, ids[0], 4, &reference) && reference.id == 4 && reference.spoiler &&
        reference.description.bytes == 8 && reference.filename.bytes == 9,
        "completed upload exposes filename, description and spoiler through typed message reference");
    check(!message_attachment_drafts_mark_queued(&store, {ids, 2}, 100) &&
        message_attachment_drafts_get(&store, ids[0])->phase == MessageAttachmentDraftPhase::ready,
        "one unfinished upload cannot partially queue a multi-file message");
    check(message_attachment_drafts_describe(&store, ids[0], {}, false) &&
        message_attachment_drafts_reference(&store, ids[0], 0, &reference) && !reference.description.bytes && !reference.spoiler,
        "clearing staged description also clears old metadata");
    check(message_attachment_drafts_uploaded(&store, ids[1], utf8_literal("uploads/second.txt")) &&
        message_attachment_drafts_mark_queued(&store, {ids, 2}, 100) &&
        !message_attachment_drafts_channel(&store, 7, {}) && store.count == 2 &&
        !message_attachment_drafts_describe(&store, ids[0], utf8_literal("changed"), false),
        "queued files leave the draft list and keep immutable metadata until message completion");
    check(!message_attachment_drafts_remove_nonce(&store, 0) && !message_attachment_drafts_remove_nonce(&store, 101) &&
        message_attachment_drafts_remove_nonce(&store, 100) == 2 && !store.count && !store.committed_bytes &&
        !message_attachment_drafts_get(&store, ids[0]),
        "completion releases only matching queued files and decommits their payload pages");
    const u64 old = ids[0];
    check(message_attachment_drafts_add(&store, 7, {files, 1}, {ids, 10}) == MessageAttachmentDraftError::none &&
        ids[0] != old && !message_attachment_drafts_get(&store, old),
        "reused attachment slot rejects stale upload completions");
    MessageAttachmentFile many[10]{};
    for (auto& file : many) file = files[1];
    check(message_attachment_drafts_add(&store, 7, {many, 10}, {ids, 10}) == MessageAttachmentDraftError::channel_capacity && store.count == 1 &&
        message_attachment_drafts_add(&store, 8, {many, 10}, {ids, 10}) == MessageAttachmentDraftError::none &&
        message_attachment_drafts_channel(&store, 8, {}) == 10,
        "per-channel ten-file bound is independent across channels");
    message_attachment_drafts_release(&store);
    check(!store.payload && !store.count && !store.committed_bytes, "attachment draft release removes every retained slot");
    if (message_attachment_drafts_initialize(&store)) {
        u64 original[2]{}, replacement = 0, ordered[2]{};
        check(message_attachment_drafts_add(&store, 7, {files, 2}, {original, 2}) == MessageAttachmentDraftError::none &&
            message_attachment_drafts_remove(&store, original[0]) &&
            message_attachment_drafts_add(&store, 7, {files, 1}, {&replacement, 1}) == MessageAttachmentDraftError::none &&
            message_attachment_drafts_channel(&store, 7, {ordered, 2}) == 2 && ordered[0] == original[1] && ordered[1] == replacement,
            "newly selected files append after surviving files even when a lower slot is reused");
        message_attachment_drafts_release(&store);
    } else check(false, "attachment ordering fixture initializes");
}
}

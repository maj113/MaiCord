#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/message_create.hpp"
#include "maicord/app/https_file_upload.hpp"
#include "maicord/app/message_attachment_storage.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/core/time.hpp"
#include "../src/app/application_attachments_state.hpp"
#include "../src/app/application_composer_text_input_state.hpp"
#include "maicord/app/account_host.hpp"
#include "../tests/file_drop_fixture.hpp"

namespace maicord {
u64 benchmark_local_attachment_roundtrip(u32 iterations, u32* peak_text, u32* record_bytes, u64* checksum, bool restore, bool history) noexcept;
u64 benchmark_message_response_roundtrip(u32 iterations, u64* checksum) noexcept;
u64 benchmark_attachment_menu(u32 iterations, u64* checksum, u32* committed_bytes) noexcept;
namespace {
void write(const char* name, u64 value) noexcept {
    char output[160]{}; u32 used = 0;
    while (*name && used < 100) output[used++] = *name++;
    output[used++] = '=';
    char digits[24]{}; u32 count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) output[used++] = digits[--count];
    output[used++] = '\n';
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, used, &written, nullptr);
}
int run() noexcept {
    constexpr u32 iterations = 100000;
    const Clock clock = clock_initialize();
    u8 target[128]{}, body[4096]{};
    char url[1024]{}, name[1024]{};
    const UploadedAttachment reference{utf8_literal("sample.txt"), utf8_literal("uploads/id/sample.txt"), {}, 0, false};
    constexpr auto response = utf8_literal("{\"attachments\":[{\"id\":\"0\",\"upload_url\":\"https://uploads.example.com/file?sig=opaque%2Ftoken\",\"upload_filename\":\"uploads/id/sample.txt\"}]}");
    u64 checksum = 0;
    const i64 start = clock_now();
    for (u32 i = 0; i < iterations; ++i) {
        const auto request = attachment_upload_write(123, i, reference.filename, 65536 + i, {target, sizeof(target)}, {body, sizeof(body)});
        const auto decoded = attachment_upload_parse_response(response, 0, {url, sizeof(url)}, {name, sizeof(name)});
        const auto message = write_message_create_body({}, i + 1, 0, 0, {&reference, 1}, {body, sizeof(body)});
        if (!request.ok || !decoded.ok || !message.ok()) return 1;
        checksum += request.body_bytes + decoded.url_bytes + message.bytes;
    }
    write("attachment_codec_iterations", iterations);
    write("attachment_codec_us", clock_microseconds(clock, start, clock_now()));
    write("attachment_codec_checksum", checksum);
    write("https_file_upload_owner_bytes", sizeof(HttpsFileUpload));
    write("network_file_owner_bytes", sizeof(NetworkFile));
    write("network_completion_bytes", sizeof(NetworkCompletion));
    write("application_attachments_bytes", sizeof(app_detail::ApplicationAttachments));
    write("message_composer_scene_bytes", sizeof(MessageComposerScene));
    write("composer_native_input_owner_bytes", sizeof(app_detail::ApplicationComposerTextInput));
    write("account_host_bytes", sizeof(AccountHost));
    write("account_module_api_bytes", sizeof(AccountModuleApi));
    u8 packed[256]{};
    UploadedAttachment restored[1]{};
    const u32 packed_bytes = message_attachment_storage_bytes({&reference, 1});
    const i64 storage_start = clock_now();
    for (u32 i = 0; i < iterations; ++i) {
        u32 count = 0;
        if (!message_attachment_storage_write({&reference, 1}, {packed, sizeof(packed)}) ||
            !message_attachment_storage_read({packed, packed_bytes}, {restored, 1}, &count) || count != 1) return 5;
        checksum += restored[0].filename.bytes + restored[0].uploaded_filename.bytes;
    }
    write("message_attachment_storage_cycle_us", clock_microseconds(clock, storage_start, clock_now()));
    write("message_attachment_storage_iterations", iterations);
    write("message_attachment_storage_bytes", packed_bytes);
    write("message_composer_request_bytes", sizeof(MessageComposerRequest));
    write("message_composer_submit_bytes", sizeof(MessageComposerSubmit));
    write("message_composer_controller_bytes", sizeof(MessageComposerController));
    MessageAttachmentDraftStore drafts{};
    if (!message_attachment_drafts_initialize(&drafts)) return 6;
    constexpr wchar_t draft_path[] = L"C:\\fixtures\\sample.txt";
    const MessageAttachmentFile draft_file{{draft_path, sizeof(draft_path) / sizeof(wchar_t) - 1}, {{1, 2, 3, 0}, 65536}};
    const i64 draft_start = clock_now();
    constexpr u32 draft_iterations = 10000;
    bool drafts_ok = true;
    for (u32 i = 0; i < draft_iterations; ++i) {
        u64 id = 0;
        if (message_attachment_drafts_add(&drafts, 7, {&draft_file, 1}, {&id, 1}) != MessageAttachmentDraftError::none ||
            !message_attachment_drafts_uploaded(&drafts, id, reference.uploaded_filename) ||
            !message_attachment_drafts_mark_queued(&drafts, {&id, 1}, i + 1) ||
            message_attachment_drafts_remove_nonce(&drafts, i + 1) != 1) { drafts_ok = false; break; }
        checksum += id;
    }
    write("attachment_draft_cycles", draft_iterations);
    write("attachment_draft_cycle_us", clock_microseconds(clock, draft_start, clock_now()));
    write("attachment_draft_record_bytes", sizeof(MessageAttachmentDraft));
    write("attachment_draft_store_bytes", sizeof(MessageAttachmentDraftStore));
    write("attachment_draft_payload_reserved_bytes", message_attachment_drafts_reserved_bytes());
    write("attachment_draft_payload_peak_committed_bytes", drafts.peak_committed_bytes);
    write("attachment_draft_payload_final_committed_bytes", drafts.committed_bytes);
    message_attachment_drafts_release(&drafts);
    if (!drafts_ok) return 7;

    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mcu", 0, path)) return 2;
    auto* chunk = static_cast<u8*>(VirtualAlloc(nullptr, kHttpsFileUploadChunkBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!chunk) { DeleteFileW(path); return 3; }
    for (u32 i = 0; i < kHttpsFileUploadChunkBytes; ++i) chunk[i] = static_cast<u8>(i);
    HANDLE seed = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    bool ok = seed != INVALID_HANDLE_VALUE;
    constexpr u32 chunks = 1024;
    for (u32 i = 0; ok && i < chunks; ++i) {
        DWORD written = 0;
        ok = WriteFile(seed, chunk, kHttpsFileUploadChunkBytes, &written, nullptr) && written == kHttpsFileUploadChunkBytes;
    }
    if (seed != INVALID_HANDLE_VALUE) CloseHandle(seed);
    NetworkLoop loop{}; NetworkFile file{};
    ok = ok && network_loop_initialize(&loop).ok() && network_file_open(&loop, &file, path).ok();
    const i64 read_start = clock_now();
    for (u32 i = 0; ok && i < chunks; ++i) {
        ok = network_file_read(&file, static_cast<u64>(i) * kHttpsFileUploadChunkBytes, {chunk, kHttpsFileUploadChunkBytes}).ok();
        if (ok) {
            const auto completed = network_loop_poll(&loop, 5000);
            ok = completed.state == NetworkPollState::completion && completed.completion.succeeded &&
                completed.completion.file == &file && completed.completion.transferred == kHttpsFileUploadChunkBytes;
            checksum += chunk[i % kHttpsFileUploadChunkBytes];
        }
    }
    const u64 read_us = clock_microseconds(clock, read_start, clock_now());
    network_file_close(&file);
    while (file.pending) network_loop_poll(&loop, 100);
    network_loop_release(&loop);
    u32 path_units = 0; while (path[path_units]) ++path_units;
    const ConstSlice<wchar_t> dropped_path{path, path_units};
    constexpr u32 drop_cycles = 100;
    u64 drop_submit_us = 0, drop_complete_us = 0, drop_join_us = 0;
    const i64 drop_start = clock_now();
    for (u32 i = 0; ok && i < drop_cycles; ++i) {
        NativeFilePicker selection{};
        const HDROP drop = file_drop_fixture::make({&dropped_path, 1});
        const i64 submitted_at = clock_now();
        const auto accepted = native_file_picker_take_drop(&selection, drop, 1);
        drop_submit_us += clock_microseconds(clock, submitted_at, clock_now());
        if (accepted != NativeFilePickerError::none) { if (drop) DragFinish(drop); ok = false; break; }
        ok = file_drop_fixture::await(&selection) && native_file_picker_status(&selection).ok() &&
            native_file_picker_status(&selection).count == 1;
        drop_complete_us += clock_microseconds(clock, submitted_at, clock_now());
        FileSnapshot snapshot{};
        ok = ok && native_file_picker_snapshot(&selection, 0, &snapshot) && snapshot.bytes == 67108864;
        // This benchmark thread can wait; the application still reaps with
        // timeout zero. Exclude the fixture's Sleep(1) polling quantum.
        const i64 joined_at = clock_now();
        const bool joined = WaitForSingleObject(selection.thread, 10000) == WAIT_OBJECT_0;
        ok = joined && ok;
        if (!native_file_picker_release(&selection, 0)) { file_drop_fixture::reap(&selection); ok = false; }
        drop_join_us += clock_microseconds(clock, joined_at, clock_now());
    }
    const u64 drop_us = clock_microseconds(clock, drop_start, clock_now());
    VirtualFree(chunk, 0, MEM_RELEASE); DeleteFileW(path);
    if (!ok) return 4;
    write("cached_file_read_bytes", static_cast<u64>(chunks) * kHttpsFileUploadChunkBytes);
    write("cached_file_read_us", read_us);
    write("file_drop_cycles", drop_cycles);
    write("file_drop_cycle_us", drop_us);
    write("file_drop_submit_us", drop_submit_us);
    write("file_drop_completion_us", drop_complete_us);
    write("file_drop_join_us", drop_join_us);
    write("checksum", checksum);
    u32 local_peak_text = 0, local_record_bytes = 0;
    const u64 local_us = benchmark_local_attachment_roundtrip(10000, &local_peak_text, &local_record_bytes, &checksum, false, false);
    if (!local_us) return 8;
    write("local_attachment_roundtrips", 10000);
    write("local_attachment_roundtrip_us", local_us);
    write("local_attachment_rich_bank_peak_used_bytes", local_peak_text);
    write("local_attachment_rich_record_bytes", local_record_bytes);
    write("local_attachment_checksum", checksum);
    const u64 restore_us = benchmark_local_attachment_roundtrip(10000, &local_peak_text, &local_record_bytes, &checksum, true, false);
    if (!restore_us) return 9;
    write("restored_attachment_roundtrip_us", restore_us);
    write("restored_attachment_checksum", checksum);
    const u64 history_us = benchmark_local_attachment_roundtrip(10000, &local_peak_text, &local_record_bytes, &checksum, false, true);
    if (!history_us) return 1;
    write("history_confirmed_attachment_roundtrip_us", history_us);
    write("history_confirmed_attachment_checksum", checksum);
    const u64 response_us = benchmark_message_response_roundtrip(10000, &checksum);
    if (!response_us) return 10;
    write("post_response_attachment_roundtrip_us", response_us);
    write("post_response_attachment_checksum", checksum);
    u32 menu_committed = 0;
    const u64 menu_us = benchmark_attachment_menu(10000, &checksum, &menu_committed);
    if (!menu_us) return 11;
    write("attachment_menu_cycles", 10000);
    write("attachment_menu_toggle_layout_us", menu_us);
    write("attachment_menu_payload_committed_bytes", menu_committed);
    write("attachment_menu_checksum", checksum);
    return 0;
}
}
}
extern "C" void maicord_attachment_upload_benchmark_entry() noexcept {
    const int result = maicord::run();
    if (result) maicord::write("failure_stage", result);
    ExitProcess(static_cast<UINT>(result));
}

#include "maicord/core/win32_process.hpp"
#include "maicord/app/message_draft_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'D', 'R', 'F', 'T'};
inline constexpr u32 kHeaderBytes = 56;
inline constexpr u32 kRecordBytes = 48;
inline constexpr u32 kSuppressReplyMention = 1;
inline constexpr u32 kChecksumOffset = 48;

u32 path_length(const wchar_t* path) noexcept {
    return wide_length(path, kAccountProfileMaximumPathCharacters);
}

MessageDraftPersistenceResult success(u32 bytes, u32 records, u32 discarded = 0) noexcept {
    return MessageDraftPersistenceResult{bytes, records, discarded, 0, MessageDraftPersistenceError::none};
}

MessageDraftPersistenceResult failure(MessageDraftPersistenceError error, u32 native_error = 0,
                                      u32 bytes = 0) noexcept {
    return MessageDraftPersistenceResult{bytes, 0, 0, native_error, error};
}

bool text_boundary(Utf8View text, u32 offset) noexcept {
    return offset <= text.bytes &&
           (offset == 0 || offset == text.bytes || (static_cast<u8>(text.data[offset]) & 0xC0u) != 0x80u);
}

bool store_valid(const MessageDraftStore& store) noexcept {
    if (!store.records || !store.text || store.capacity == 0 ||
        store.capacity > kMessageDraftStoreMaximumRecords || store.count > store.capacity ||
        store.text_used > store.text_capacity || store.text_wasted > store.text_used) {
        return false;
    }
    u32 occupied = 0;
    for (u32 index = 0; index < store.capacity; ++index) {
        const MessageDraftRecord& record = store.records[index];
        if (!record.occupied) {
            continue;
        }
        ++occupied;
        if (record.channel_id == 0 || (record.text_bytes == 0 && record.reply_message_id == 0) ||
            (record.reply_message_id == 0 && !record.mention_reply_author) || record.touched == 0 ||
            record.touched > store.clock || record.revision == 0 || store.revision == 0 ||
            record.text_offset > store.text_used || record.text_bytes > store.text_used - record.text_offset) {
            return false;
        }
        for (u32 previous = 0; previous < index; ++previous) {
            if (store.records[previous].occupied && store.records[previous].channel_id == record.channel_id) {
                return false;
            }
        }
        const Utf8View text{reinterpret_cast<const char*>(store.text + record.text_offset),
                            record.text_bytes};
        if (!utf8_validate(text).valid || !text_boundary(text, record.anchor) ||
            !text_boundary(text, record.caret)) {
            return false;
        }
    }
    return occupied == store.count;
}

u64 checksum(const u8* bytes, u32 count) noexcept {
    return hash64_bytes(kHash64Seed, bytes, count);
}

bool transfer(HANDLE file, u8* bytes, u32 count, bool write, u32* native_error) noexcept {
    u32 offset = 0;
    while (offset < count) {
        DWORD transferred = 0;
        const BOOL ok = write ? WriteFile(file, bytes + offset, count - offset, &transferred, nullptr)
                              : ReadFile(file, bytes + offset, count - offset, &transferred, nullptr);
        if (!ok || transferred == 0) {
            *native_error = ok ? ERROR_HANDLE_EOF : thread_last_error();
            return false;
        }
        offset += transferred;
    }
    return true;
}

void erase_release(u8* bytes, u32 count) noexcept {
    if (bytes) {
        secure_clear_bytes(bytes, count);
        VirtualFree(bytes, 0, MEM_RELEASE);
    }
}

// Erases and releases the buffer on every exit of a load.
struct ErasedBuffer {
    u8* bytes;
    u32 count;
    ~ErasedBuffer() noexcept { erase_release(bytes, count); }
};

bool trim_whitespace(u32 codepoint) noexcept {
    return (codepoint >= 0x0009 && codepoint <= 0x000D) || codepoint == 0x0020 || codepoint == 0x00A0 ||
           codepoint == 0x1680 || (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 ||
           codepoint == 0x2029 || codepoint == 0x202F || codepoint == 0x205F || codepoint == 0x3000 ||
           codepoint == 0xFEFF;
}

bool blank_after_trim(Utf8View text) noexcept {
    u32 offset = 0;
    while (offset < text.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid || !trim_whitespace(decoded.codepoint)) {
            return false;
        }
        offset += decoded.width;
    }
    return true;
}

bool records_duplicate(const u8* records, u32 record_bytes, u32 record_index, u64 channel_id) noexcept {
    for (u32 previous = 0; previous < record_index; ++previous) {
        if (byte_load_le64(records + previous * record_bytes) == channel_id) {
            return true;
        }
    }
    return false;
}

} // namespace

void message_drafts_image_release(MessageDraftImage* image) noexcept {
    if (image) {
        erase_release(image->bytes, image->count);
        *image = MessageDraftImage{};
    }
}

MessageDraftPersistenceResult message_drafts_encode(u64 profile_id, const MessageDraftStore& store,
                                                    MessageDraftImage* image) noexcept {
    if (!image || image->bytes || profile_id == 0 || !store_valid(store)) {
        return failure(MessageDraftPersistenceError::invalid_input);
    }
    u32 text_bytes = 0;
    for (u32 index = 0; index < store.capacity; ++index) {
        if (store.records[index].occupied) {
            if (store.records[index].text_bytes > ~text_bytes) {
                return failure(MessageDraftPersistenceError::capacity);
            }
            text_bytes += store.records[index].text_bytes;
        }
    }
    const u64 total64 =
        static_cast<u64>(kHeaderBytes) + static_cast<u64>(store.count) * kRecordBytes + text_bytes;
    if (total64 > kMessageDraftPersistenceMaximumFileBytes) {
        return failure(MessageDraftPersistenceError::capacity);
    }
    const u32 total = static_cast<u32>(total64);
    auto* file_bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!file_bytes) {
        return failure(MessageDraftPersistenceError::memory, thread_last_error());
    }
    for (u32 index = 0; index < sizeof(kMagic); ++index) {
        file_bytes[index] = kMagic[index];
    }
    byte_store_le32(file_bytes + 8, kMessageDraftPersistenceVersion);
    byte_store_le32(file_bytes + 12, kHeaderBytes);
    byte_store_le32(file_bytes + 16, total);
    byte_store_le32(file_bytes + 20, store.count);
    byte_store_le32(file_bytes + 24, text_bytes);
    byte_store_le32(file_bytes + 28, store.revision);
    byte_store_le64(file_bytes + 32, profile_id);
    byte_store_le64(file_bytes + 40, store.clock);
    byte_store_le64(file_bytes + kChecksumOffset, 0);

    u32 record_cursor = kHeaderBytes;
    u32 text_cursor = kHeaderBytes + store.count * kRecordBytes;
    for (u32 index = 0; index < store.capacity; ++index) {
        const MessageDraftRecord& record = store.records[index];
        if (!record.occupied) {
            continue;
        }
        byte_store_le64(file_bytes + record_cursor, record.channel_id);
        byte_store_le64(file_bytes + record_cursor + 8, record.touched);
        byte_store_le32(file_bytes + record_cursor + 16, record.text_bytes);
        byte_store_le32(file_bytes + record_cursor + 20, record.anchor);
        byte_store_le32(file_bytes + record_cursor + 24, record.caret);
        byte_store_le32(file_bytes + record_cursor + 28, record.revision);
        byte_store_le64(file_bytes + record_cursor + 32, record.reply_message_id);
        byte_store_le32(file_bytes + record_cursor + 40,
                         record.mention_reply_author ? 0 : kSuppressReplyMention);
        byte_store_le32(file_bytes + record_cursor + 44, 0);
        copy_bytes(file_bytes + text_cursor, store.text + record.text_offset, record.text_bytes);
        record_cursor += kRecordBytes;
        text_cursor += record.text_bytes;
    }
    byte_store_le64(file_bytes + kChecksumOffset, checksum(file_bytes, total));
    *image = MessageDraftImage{file_bytes, total, store.count};
    return success(total, store.count);
}

MessageDraftPersistenceResult message_drafts_commit(const wchar_t* path, MessageDraftImage* image,
                                                    bool write_through) noexcept {
    const u32 characters = path_length(path);
    if (!image || !image->bytes || characters == 0 ||
        characters + 4 >= kAccountProfileMaximumPathCharacters) {
        message_drafts_image_release(image);
        return failure(MessageDraftPersistenceError::invalid_input);
    }
    u8* const file_bytes = image->bytes;
    const u32 total = image->count;
    const u32 records = image->records;
    *image = MessageDraftImage{};
    wchar_t temporary[kAccountProfileMaximumPathCharacters]{};
    for (u32 index = 0; index < characters; ++index) {
        temporary[index] = path[index];
    }
    temporary[characters] = L'.';
    temporary[characters + 1] = L'n';
    temporary[characters + 2] = L'e';
    temporary[characters + 3] = L'w';
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY |
                                  (write_through ? FILE_FLAG_WRITE_THROUGH : 0),
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 native_error = thread_last_error();
        erase_release(file_bytes, total);
        return failure(MessageDraftPersistenceError::open, native_error);
    }
    u32 native_error = 0;
    const bool written = transfer(file, file_bytes, total, true, &native_error);
    const bool flushed = written && (!write_through || FlushFileBuffers(file));
    if (written && !flushed) {
        native_error = thread_last_error();
    }
    CloseHandle(file);
    erase_release(file_bytes, total);
    if (!written || !flushed) {
        DeleteFileW(temporary);
        return failure(written ? MessageDraftPersistenceError::flush : MessageDraftPersistenceError::write,
                       native_error);
    }
    if (!MoveFileExW(temporary, path,
                     MOVEFILE_REPLACE_EXISTING | (write_through ? MOVEFILE_WRITE_THROUGH : 0))) {
        native_error = thread_last_error();
        DeleteFileW(temporary);
        return failure(MessageDraftPersistenceError::commit, native_error);
    }
    return success(total, records);
}

MessageDraftPersistenceResult message_drafts_save_file(const wchar_t* path, u64 profile_id,
                                                       const MessageDraftStore& store) noexcept {
    const u32 characters = path_length(path);
    if (characters == 0 || characters + 4 >= kAccountProfileMaximumPathCharacters) {
        return failure(MessageDraftPersistenceError::invalid_input);
    }
    MessageDraftImage image{};
    const MessageDraftPersistenceResult encoded = message_drafts_encode(profile_id, store, &image);
    return encoded.ok() ? message_drafts_commit(path, &image, true) : encoded;
}

MessageDraftPersistenceResult message_drafts_load_file(const wchar_t* path, u64 expected_profile_id,
                                                       MessageDraftStore* store) noexcept {
    const u32 characters = path_length(path);
    if (characters == 0 || characters >= kAccountProfileMaximumPathCharacters || expected_profile_id == 0 ||
        !store || !store_valid(*store)) {
        return failure(MessageDraftPersistenceError::invalid_input);
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return failure(MessageDraftPersistenceError::open, thread_last_error());
    }
    Win32LargeInteger size{};
    if (!GetFileSizeEx(file, size.native()) || size.QuadPart < kHeaderBytes ||
        size.QuadPart > kMessageDraftPersistenceMaximumFileBytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(MessageDraftPersistenceError::size, native_error);
    }
    const u32 file_size = static_cast<u32>(size.QuadPart);
    auto* file_bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, file_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!file_bytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(MessageDraftPersistenceError::memory, native_error);
    }
    const ErasedBuffer file_guard{file_bytes, file_size};
    u32 native_error = 0;
    const bool read = transfer(file, file_bytes, file_size, false, &native_error);
    CloseHandle(file);
    if (!read) {
        return failure(MessageDraftPersistenceError::read, native_error);
    }
    bool magic = true;
    for (u32 index = 0; index < sizeof(kMagic); ++index) {
        magic = magic && file_bytes[index] == kMagic[index];
    }
    if (!magic) {
        return failure(MessageDraftPersistenceError::format);
    }
    const u32 version = byte_load_le32(file_bytes + 8);
    if (version != kMessageDraftPersistenceVersion) {
        return failure(MessageDraftPersistenceError::version);
    }
    if (byte_load_le64(file_bytes + 32) != expected_profile_id) {
        return failure(MessageDraftPersistenceError::profile);
    }
    const u32 record_count = byte_load_le32(file_bytes + 20);
    constexpr u32 record_bytes = kRecordBytes;
    const u32 text_bytes = byte_load_le32(file_bytes + 24);
    const u32 stored_revision = byte_load_le32(file_bytes + 28);
    const u64 stored_clock = byte_load_le64(file_bytes + 40);
    const u64 stored_checksum = byte_load_le64(file_bytes + kChecksumOffset);
    byte_store_le64(file_bytes + kChecksumOffset, 0);
    const u64 actual_checksum = checksum(file_bytes, file_size);
    byte_store_le64(file_bytes + kChecksumOffset, stored_checksum);
    const u64 expected_size =
        static_cast<u64>(kHeaderBytes) + static_cast<u64>(record_count) * record_bytes + text_bytes;
    if (byte_load_le32(file_bytes + 12) != kHeaderBytes || byte_load_le32(file_bytes + 16) != file_size ||
        record_count > kMessageDraftStoreMaximumRecords || expected_size != file_size ||
        actual_checksum != stored_checksum || (record_count != 0 && stored_revision == 0)) {
        return failure(MessageDraftPersistenceError::format);
    }

    const u8* records = file_bytes + kHeaderBytes;
    const u8* texts = records + record_count * record_bytes;
    u32 text_cursor = 0;
    u32 accepted_records = 0;
    u32 accepted_bytes = 0;
    u32 discarded = 0;
    for (u32 index = 0; index < record_count; ++index) {
        const u8* record = records + index * record_bytes;
        const u64 channel_id = byte_load_le64(record);
        const u64 touched = byte_load_le64(record + 8);
        const u32 bytes = byte_load_le32(record + 16);
        const u32 anchor = byte_load_le32(record + 20);
        const u32 caret = byte_load_le32(record + 24);
        const u32 revision = byte_load_le32(record + 28);
        const u64 reply_message_id = byte_load_le64(record + 32);
        const u32 flags = byte_load_le32(record + 40);
        const bool reserved_valid = byte_load_le32(record + 44) == 0;
        if (channel_id == 0 || touched == 0 || touched > stored_clock ||
            (bytes == 0 && reply_message_id == 0) ||
            text_cursor > text_bytes || bytes > text_bytes - text_cursor || revision == 0 ||
            !reserved_valid || (flags & ~kSuppressReplyMention) != 0 ||
            (reply_message_id == 0 && flags != 0) ||
            records_duplicate(records, record_bytes, index, channel_id)) {
            return failure(MessageDraftPersistenceError::format);
        }
        const Utf8View text{reinterpret_cast<const char*>(texts + text_cursor), bytes};
        if (!utf8_validate(text).valid || !text_boundary(text, anchor) || !text_boundary(text, caret)) {
            return failure(MessageDraftPersistenceError::format);
        }
        if (reply_message_id == 0 && blank_after_trim(text)) {
            ++discarded;
        } else {
            ++accepted_records;
            accepted_bytes += bytes;
        }
        text_cursor += bytes;
    }
    if (text_cursor != text_bytes || accepted_records > store->capacity ||
        accepted_bytes > store->text_capacity) {
        return failure(MessageDraftPersistenceError::capacity);
    }

    const u64 scratch64 =
        static_cast<u64>(store->capacity) * sizeof(MessageDraftRecord) + store->text_capacity;
    if (scratch64 > 0xFFFFFFFFull) {
        return failure(MessageDraftPersistenceError::capacity);
    }
    const u32 scratch_bytes = static_cast<u32>(scratch64);
    auto* scratch =
        static_cast<u8*>(VirtualAlloc(nullptr, scratch_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!scratch) {
        return failure(MessageDraftPersistenceError::memory, thread_last_error());
    }
    const ErasedBuffer scratch_guard{scratch, scratch_bytes};
    auto* temporary_records = reinterpret_cast<MessageDraftRecord*>(scratch);
    auto* temporary_text = scratch + store->capacity * sizeof(MessageDraftRecord);
    MessageDraftStore temporary{};
    if (!message_draft_store_initialize(&temporary,
                                        Slice<MessageDraftRecord>{temporary_records, store->capacity},
                                        Slice<u8>{temporary_text, store->text_capacity})) {
        return failure(MessageDraftPersistenceError::capacity);
    }
    text_cursor = 0;
    for (u32 index = 0; index < record_count; ++index) {
        const u8* encoded = records + index * record_bytes;
        const u32 bytes = byte_load_le32(encoded + 16);
        const Utf8View text{reinterpret_cast<const char*>(texts + text_cursor), bytes};
        const u64 reply_message_id = byte_load_le64(encoded + 32);
        const bool mention_reply_author = (byte_load_le32(encoded + 40) & kSuppressReplyMention) == 0;
        if (reply_message_id != 0 || !blank_after_trim(text)) {
            const MessageDraftStoreResult saved = message_draft_store_save(
                &temporary, MessageDraftInput{byte_load_le64(encoded), text, byte_load_le32(encoded + 20),
                                              byte_load_le32(encoded + 24), reply_message_id,
                                              mention_reply_author});
            if (!saved.ok()) {
                return failure(MessageDraftPersistenceError::capacity);
            }
            temporary.records[saved.index].touched = byte_load_le64(encoded + 8);
            temporary.records[saved.index].revision = byte_load_le32(encoded + 28);
        }
        text_cursor += bytes;
    }
    temporary.clock = stored_clock;
    temporary.revision = stored_revision;
    temporary.saved = 0;
    temporary.deleted = 0;
    temporary.compactions = 0;
    temporary.rejected = 0;
    copy_bytes(reinterpret_cast<u8*>(store->records), reinterpret_cast<const u8*>(temporary.records),
               store->capacity * sizeof(MessageDraftRecord));
    copy_bytes(store->text, temporary.text, store->text_capacity);
    temporary.records = store->records;
    temporary.text = store->text;
    *store = temporary;

    return success(file_size, accepted_records, discarded);
}

} // namespace maicord

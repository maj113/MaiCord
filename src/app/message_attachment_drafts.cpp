#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
namespace maicord {
namespace {
constexpr u32 kPayloadBytes = ((kNativeFilePickerMaximumPathUnits + 1) * sizeof(wchar_t) +
    kAttachmentUploadMaximumFilenameBytes + kAttachmentUploadMaximumUploadedFilenameBytes + 4096 + 4095) & ~4095u;
u64 identifier(const MessageAttachmentDraft& record, u32 slot) noexcept {
    return (static_cast<u64>(record.generation) << 32) | (slot + 1);
}
u8* payload(const MessageAttachmentDraftStore* store, u64 id) noexcept {
    return store->payload + (static_cast<u32>(id) - 1) * kPayloadBytes;
}
u32 name_offset(const MessageAttachmentDraft& record) noexcept { return (record.path_units + 1) * sizeof(wchar_t); }
bool commit(MessageAttachmentDraftStore* store, MessageAttachmentDraft* record, u64 id, u32 bytes) noexcept {
    const u32 rounded = (bytes + 4095) & ~4095u;
    if (rounded > kPayloadBytes) return false;
    if (rounded > record->committed_bytes) {
        const u32 delta = rounded - record->committed_bytes;
        if (!VirtualAlloc(payload(store, id) + record->committed_bytes, delta, MEM_COMMIT, PAGE_READWRITE)) return false;
        record->committed_bytes = rounded;
        store->committed_bytes += delta;
        store->peak_committed_bytes = maximum(store->committed_bytes, store->peak_committed_bytes);
    }
    return true;
}
}
u32 message_attachment_drafts_reserved_bytes() noexcept { return kPayloadBytes * kMessageAttachmentDraftCapacity; }
bool message_attachment_drafts_initialize(MessageAttachmentDraftStore* store) noexcept {
    if (!store || store->payload) return false;
    reset_value(store);
    store->payload = static_cast<u8*>(VirtualAlloc(nullptr, message_attachment_drafts_reserved_bytes(), MEM_RESERVE, PAGE_READWRITE));
    return store->payload != nullptr;
}
void message_attachment_drafts_release(MessageAttachmentDraftStore* store) noexcept {
    if (!store) return;
    for (u32 i = 0; i < kMessageAttachmentDraftCapacity; ++i)
        if (store->records[i].occupied) message_attachment_drafts_remove(store, identifier(store->records[i], i));
    if (store->payload) VirtualFree(store->payload, 0, MEM_RELEASE);
    reset_value(store);
}
MessageAttachmentDraft* message_attachment_drafts_get(MessageAttachmentDraftStore* store, u64 id) noexcept {
    const u32 slot = static_cast<u32>(id);
    if (!store || !store->payload || !slot || slot > kMessageAttachmentDraftCapacity) return nullptr;
    auto& record = store->records[slot - 1];
    return record.occupied && record.generation == static_cast<u32>(id >> 32) ? &record : nullptr;
}
const MessageAttachmentDraft* message_attachment_drafts_get(const MessageAttachmentDraftStore* store, u64 id) noexcept {
    return message_attachment_drafts_get(const_cast<MessageAttachmentDraftStore*>(store), id);
}
u32 message_attachment_drafts_channel(const MessageAttachmentDraftStore* store, u64 channel_id, Slice<u64> ids) noexcept {
    if (!store || !store->payload || !channel_id || (!ids.data && ids.count)) return 0;
    u32 count = 0;
    for (u32 i = 0; i < store->count; ++i) {
        const u32 slot = store->order[i];
        const auto& record = store->records[slot];
        if (!record.occupied || record.voice || record.channel_id != channel_id ||
            record.phase == MessageAttachmentDraftPhase::queued) continue;
        if (count < ids.count) ids[count] = identifier(record, slot);
        ++count;
    }
    return count;
}
MessageAttachmentDraftError message_attachment_drafts_add(MessageAttachmentDraftStore* store, u64 channel_id,
    ConstSlice<MessageAttachmentFile> files, Slice<u64> ids) noexcept {
    if (!store || !store->payload || !channel_id || !files.data || !files.count ||
        files.count > kAttachmentUploadMaximumCount || !ids.data || ids.count < files.count) return MessageAttachmentDraftError::invalid_input;
    if (files.count > kAttachmentUploadMaximumCount - minimum(kAttachmentUploadMaximumCount,
        message_attachment_drafts_channel(store, channel_id, {}))) return MessageAttachmentDraftError::channel_capacity;
    if (files.count > kMessageAttachmentDraftCapacity - store->count) return MessageAttachmentDraftError::capacity;
    struct Prepared { char filename[kAttachmentUploadMaximumFilenameBytes]; u32 bytes; };
    Prepared prepared[kAttachmentUploadMaximumCount];
    for (u32 i = 0; i < files.count; ++i) {
        const auto& file = files[i];
        if ((file.content.context ? !upload_source_valid(file.content) || file.content.bytes != file.snapshot.bytes || file.path.count :
            !native_file_picker_valid_path(file.path)) || file.snapshot.bytes > 0x7fffffffffffffffull) return MessageAttachmentDraftError::invalid_input;
        if (!file.snapshot.bytes) return MessageAttachmentDraftError::empty_file;
        u32 first = file.path.count;
        while (first && file.path[first - 1] != L'\\') --first;
        const auto filename = file.content.context ? file.filename : ConstSlice<wchar_t>{file.path.data + first, file.path.count - first};
        const int bytes = filename.data && filename.count ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename.data,
            static_cast<int>(filename.count), prepared[i].filename, sizeof(prepared[i].filename), nullptr, nullptr) : 0;
        if (bytes <= 0 || !attachment_upload_valid_filename({prepared[i].filename, static_cast<u32>(bytes)})) return MessageAttachmentDraftError::invalid_input;
        prepared[i].bytes = static_cast<u32>(bytes);
    }
    u32 added = 0;
    for (u32 slot = 0; slot < kMessageAttachmentDraftCapacity && added < files.count; ++slot) {
        auto& record = store->records[slot];
        if (record.occupied) continue;
        u32 generation = record.generation + 1; if (!generation) generation = 1;
        reset_value(&record); record.generation = generation;
        record.path_units = files[added].path.count;
        record.filename_bytes = static_cast<u16>(prepared[added].bytes);
        const u64 id = identifier(record, slot);
        if (!commit(store, &record, id, name_offset(record) + record.filename_bytes)) {
            for (u32 i = 0; i < added; ++i) message_attachment_drafts_remove(store, ids[i]);
            return MessageAttachmentDraftError::memory;
        }
        copy_bytes(payload(store, id), files[added].path.data, record.path_units * sizeof(wchar_t));
        reinterpret_cast<wchar_t*>(payload(store, id))[record.path_units] = 0;
        copy_bytes(payload(store, id) + name_offset(record), prepared[added].filename, record.filename_bytes);
        record.source = files[added].snapshot; record.channel_id = channel_id;
        record.content = files[added].content; upload_source_retain(record.content);
        record.phase = MessageAttachmentDraftPhase::waiting_url; record.occupied = true;
        ids[added++] = id; store->order[store->count++] = static_cast<u8>(slot); ++store->revision;
    }
    return added == files.count ? MessageAttachmentDraftError::none : MessageAttachmentDraftError::capacity;
}
MessageAttachmentDraftError message_attachment_drafts_add_voice(MessageAttachmentDraftStore* store, u64 channel_id,
    ConstSlice<wchar_t> filename, UploadSource content, Utf8View waveform, u32 duration_ms, u64* id) noexcept {
    if (!id || !content.context || !waveform.bytes || waveform.bytes > 4096 || !duration_ms)
        return MessageAttachmentDraftError::invalid_input;
    const MessageAttachmentFile file{{}, {{}, content.bytes}, filename, content};
    const auto added = message_attachment_drafts_add(store, channel_id, {&file, 1}, {id, 1});
    if (added != MessageAttachmentDraftError::none) return added;
    auto* record = message_attachment_drafts_get(store, *id);
    // The waveform lives where a file's description would; voice drafts have none.
    const u32 offset = name_offset(*record) + record->filename_bytes + kAttachmentUploadMaximumUploadedFilenameBytes;
    if (!commit(store, record, *id, offset + waveform.bytes)) {
        message_attachment_drafts_remove(store, *id);
        return MessageAttachmentDraftError::memory;
    }
    copy_bytes(payload(store, *id) + offset, waveform.data, waveform.bytes);
    record->description_bytes = static_cast<u16>(waveform.bytes);
    record->voice = true;
    record->voice_duration_ms = duration_ms;
    ++store->revision;
    return MessageAttachmentDraftError::none;
}
ConstSlice<wchar_t> message_attachment_drafts_path(const MessageAttachmentDraftStore* store, u64 id) noexcept {
    const auto* record = message_attachment_drafts_get(store, id);
    return record ? ConstSlice<wchar_t>{reinterpret_cast<const wchar_t*>(payload(store, id)), record->path_units} : ConstSlice<wchar_t>{};
}
Utf8View message_attachment_drafts_filename(const MessageAttachmentDraftStore* store, u64 id) noexcept {
    const auto* record = message_attachment_drafts_get(store, id);
    return record ? Utf8View{reinterpret_cast<const char*>(payload(store, id) + name_offset(*record)), record->filename_bytes} : Utf8View{};
}
Utf8View message_attachment_drafts_description(const MessageAttachmentDraftStore* store, u64 id) noexcept {
    const auto* record = message_attachment_drafts_get(store, id);
    return record && !record->voice && record->description_bytes ? Utf8View{reinterpret_cast<const char*>(payload(store, id) + name_offset(*record) +
        record->filename_bytes + kAttachmentUploadMaximumUploadedFilenameBytes), record->description_bytes} : Utf8View{};
}
bool message_attachment_drafts_uploaded(MessageAttachmentDraftStore* store, u64 id, Utf8View uploaded_filename) noexcept {
    auto* record = message_attachment_drafts_get(store, id);
    if (!record || record->phase == MessageAttachmentDraftPhase::queued) return false;
    const UploadedAttachment reference{message_attachment_drafts_filename(store, id), uploaded_filename, {}, 0, record->spoiler};
    if (!attachment_upload_valid_reference(reference)) return false;
    const u32 offset = name_offset(*record) + record->filename_bytes;
    if (!commit(store, record, id, offset + uploaded_filename.bytes)) return false;
    copy_bytes(payload(store, id) + offset, uploaded_filename.data, uploaded_filename.bytes);
    if (uploaded_filename.bytes < record->uploaded_filename_bytes)
        secure_clear_bytes(payload(store, id) + offset + uploaded_filename.bytes, record->uploaded_filename_bytes - uploaded_filename.bytes);
    record->uploaded_filename_bytes = static_cast<u16>(uploaded_filename.bytes);
    record->uploaded_bytes = record->source.bytes;
    record->phase = MessageAttachmentDraftPhase::ready; ++store->revision;
    return true;
}
bool message_attachment_drafts_reference(const MessageAttachmentDraftStore* store, u64 id, u32 attachment_id,
    UploadedAttachment* output) noexcept {
    const auto* record = message_attachment_drafts_get(store, id);
    if (!record || !output || (record->phase != MessageAttachmentDraftPhase::ready && record->phase != MessageAttachmentDraftPhase::queued)) return false;
    const char* strings = reinterpret_cast<const char*>(payload(store, id) + name_offset(*record));
    const Utf8View trailing = record->description_bytes ? Utf8View{strings + record->filename_bytes +
        kAttachmentUploadMaximumUploadedFilenameBytes, record->description_bytes} : Utf8View{};
    *output = {{strings, record->filename_bytes}, {strings + record->filename_bytes, record->uploaded_filename_bytes},
        record->voice ? Utf8View{} : trailing, attachment_id, record->spoiler};
    if (record->voice) {
        output->waveform = trailing;
        output->duration_ms = record->voice_duration_ms;
    }
    return true;
}
bool message_attachment_drafts_describe(MessageAttachmentDraftStore* store, u64 id, Utf8View description, bool spoiler) noexcept {
    auto* record = message_attachment_drafts_get(store, id);
    if (!record || record->phase == MessageAttachmentDraftPhase::queued || description.bytes > 4096 || (!description.data && description.bytes)) return false;
    const auto valid = utf8_validate(description);
    if (description.bytes && (!valid.valid || valid.codepoints > 1024)) return false;
    const u32 offset = name_offset(*record) + record->filename_bytes + kAttachmentUploadMaximumUploadedFilenameBytes;
    if (description.bytes && !commit(store, record, id, offset + description.bytes)) return false;
    if (description.bytes) copy_bytes(payload(store, id) + offset, description.data, description.bytes);
    if (description.bytes < record->description_bytes)
        secure_clear_bytes(payload(store, id) + offset + description.bytes, record->description_bytes - description.bytes);
    record->description_bytes = static_cast<u16>(description.bytes); record->spoiler = spoiler; ++store->revision;
    return true;
}
bool message_attachment_drafts_set_spoiler(MessageAttachmentDraftStore* store, u64 id, bool spoiler) noexcept {
    auto* record = message_attachment_drafts_get(store, id);
    if (!record || record->phase == MessageAttachmentDraftPhase::queued) return false;
    if (record->spoiler != spoiler) { record->spoiler = spoiler; ++store->revision; }
    return true;
}
bool message_attachment_drafts_rename(MessageAttachmentDraftStore* store, u64 id, Utf8View filename) noexcept {
    auto* record = message_attachment_drafts_get(store, id);
    if (!record || record->phase == MessageAttachmentDraftPhase::queued || !attachment_upload_valid_filename(filename))
        return false;
    u8* strings = payload(store, id) + name_offset(*record);
    if (filename.bytes == record->filename_bytes && bytes_equal(strings, filename.data, filename.bytes)) return true;
    // The uploaded name and description follow the filename; stage both.
    char uploaded[kAttachmentUploadMaximumUploadedFilenameBytes];
    char description[4096];
    const u32 uploaded_bytes = record->uploaded_filename_bytes, description_bytes = record->description_bytes;
    copy_bytes(uploaded, strings + record->filename_bytes, uploaded_bytes);
    copy_bytes(description, strings + record->filename_bytes + kAttachmentUploadMaximumUploadedFilenameBytes, description_bytes);
    const u32 old_end = record->filename_bytes + (description_bytes ? kAttachmentUploadMaximumUploadedFilenameBytes + description_bytes : uploaded_bytes);
    const u32 new_end = filename.bytes + (description_bytes ? kAttachmentUploadMaximumUploadedFilenameBytes + description_bytes : uploaded_bytes);
    bool ok = commit(store, record, id, name_offset(*record) + new_end);
    if (ok) {
        secure_clear_bytes(strings, old_end);
        copy_bytes(strings, filename.data, filename.bytes);
        copy_bytes(strings + filename.bytes, uploaded, uploaded_bytes);
        copy_bytes(strings + filename.bytes + kAttachmentUploadMaximumUploadedFilenameBytes, description, description_bytes);
        record->filename_bytes = static_cast<u16>(filename.bytes); ++store->revision;
    }
    secure_clear_bytes(uploaded, sizeof(uploaded)); secure_clear_bytes(description, sizeof(description));
    return ok;
}
bool message_attachment_drafts_mark_queued(MessageAttachmentDraftStore* store, ConstSlice<u64> ids, u64 nonce) noexcept {
    if (!store || !ids.data || !ids.count || ids.count > kAttachmentUploadMaximumCount || !nonce) return false;
    u64 channel = 0;
    for (u32 i = 0; i < ids.count; ++i) {
        const auto* record = message_attachment_drafts_get(store, ids[i]);
        if (!record || record->phase != MessageAttachmentDraftPhase::ready || (channel && record->channel_id != channel)) return false;
        for (u32 j = 0; j < i; ++j) if (ids[j] == ids[i]) return false;
        channel = record->channel_id;
    }
    for (u32 i = 0; i < ids.count; ++i) {
        auto* record = message_attachment_drafts_get(store, ids[i]);
        record->phase = MessageAttachmentDraftPhase::queued; record->nonce = nonce;
    }
    ++store->revision; return true;
}
u32 message_attachment_drafts_remove_nonce(MessageAttachmentDraftStore* store, u64 nonce) noexcept {
    if (!store || !nonce) return 0;
    u32 removed = 0;
    for (u32 i = 0; i < kMessageAttachmentDraftCapacity; ++i) {
        const auto& record = store->records[i];
        if (record.occupied && record.phase == MessageAttachmentDraftPhase::queued && record.nonce == nonce &&
            message_attachment_drafts_remove(store, identifier(record, i))) ++removed;
    }
    return removed;
}
bool message_attachment_drafts_remove(MessageAttachmentDraftStore* store, u64 id) noexcept {
    auto* record = message_attachment_drafts_get(store, id);
    if (!record) return false;
    secure_clear_bytes(payload(store, id), record->committed_bytes);
    if (!VirtualFree(payload(store, id), record->committed_bytes, MEM_DECOMMIT)) return false;
    store->committed_bytes -= record->committed_bytes;
    const u32 generation = record->generation;
    upload_source_release(record->content);
    reset_value(record); record->generation = generation;
    const u32 slot = static_cast<u32>(id) - 1;
    for (u32 i = 0; i < store->count; ++i) {
        if (store->order[i] != slot) continue;
        for (u32 j = i + 1; j < store->count; ++j) store->order[j - 1] = store->order[j];
        store->order[store->count - 1] = 0;
        break;
    }
    --store->count; ++store->revision;
    return true;
}
}

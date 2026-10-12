#pragma once
#include "maicord/platform/file_picker.hpp"
#include "maicord/protocol/attachment_upload.hpp"
namespace maicord {
inline constexpr u32 kMessageAttachmentDraftCapacity = 64;
enum class MessageAttachmentDraftPhase : u8 { waiting_url, uploading, ready, queued, failed };
enum class MessageAttachmentDraftFailure : u8 { none, request, upload, source_changed, memory, size_limit };
enum class MessageAttachmentDraftError : u8 { none, invalid_input, channel_capacity, capacity, empty_file, memory };
struct MessageAttachmentFile { ConstSlice<wchar_t> path; FileSnapshot snapshot; ConstSlice<wchar_t> filename; UploadSource content; };
struct MessageAttachmentDraft {
    UploadSource content;
    FileSnapshot source;
    u64 channel_id;
    u64 nonce;
    u64 retry_at_ms;
    u64 uploaded_bytes;
    u32 generation;
    u32 committed_bytes;
    u32 path_units;
    u16 filename_bytes;
    u16 uploaded_filename_bytes;
    u16 description_bytes;
    u16 http_status;
    MessageAttachmentDraftPhase phase;
    bool spoiler;
    bool occupied;
    MessageAttachmentDraftFailure failure;
    // A recorded voice message: uploaded like a file but never listed with the
    // channel's drafts; its reference carries the waveform and duration.
    bool voice;
    u32 voice_duration_ms;
};
struct MessageAttachmentDraftStore {
    u8* payload;
    MessageAttachmentDraft records[kMessageAttachmentDraftCapacity];
    u8 order[kMessageAttachmentDraftCapacity];
    u32 count;
    u32 revision;
    u32 committed_bytes;
    u32 peak_committed_bytes;
};
bool message_attachment_drafts_initialize(MessageAttachmentDraftStore* store) noexcept;
void message_attachment_drafts_release(MessageAttachmentDraftStore* store) noexcept;
// Batch is atomic and copied. No file/Shell operations run here. Per-channel
// admission excludes queued sends, while all retained records share one bound.
MessageAttachmentDraftError message_attachment_drafts_add(MessageAttachmentDraftStore* store, u64 channel_id,
    ConstSlice<MessageAttachmentFile> files, Slice<u64> ids) noexcept;
MessageAttachmentDraft* message_attachment_drafts_get(MessageAttachmentDraftStore* store, u64 id) noexcept;
const MessageAttachmentDraft* message_attachment_drafts_get(const MessageAttachmentDraftStore* store, u64 id) noexcept;
// The channel's file drafts (voice messages excluded), in order.
u32 message_attachment_drafts_channel(const MessageAttachmentDraftStore* store, u64 channel_id, Slice<u64> ids) noexcept;
// One in-memory voice message upload (filename, Ogg source, base64 waveform
// and audible duration); it shares the URL/upload pipeline with files.
MessageAttachmentDraftError message_attachment_drafts_add_voice(MessageAttachmentDraftStore* store, u64 channel_id,
    ConstSlice<wchar_t> filename, UploadSource content, Utf8View waveform, u32 duration_ms, u64* id) noexcept;
ConstSlice<wchar_t> message_attachment_drafts_path(const MessageAttachmentDraftStore* store, u64 id) noexcept;
Utf8View message_attachment_drafts_filename(const MessageAttachmentDraftStore* store, u64 id) noexcept;
Utf8View message_attachment_drafts_description(const MessageAttachmentDraftStore* store, u64 id) noexcept;
bool message_attachment_drafts_uploaded(MessageAttachmentDraftStore* store, u64 id, Utf8View uploaded_filename) noexcept;
bool message_attachment_drafts_describe(MessageAttachmentDraftStore* store, u64 id, Utf8View description, bool spoiler) noexcept;
bool message_attachment_drafts_set_spoiler(MessageAttachmentDraftStore* store, u64 id, bool spoiler) noexcept;
// Display filename sent with the message; the uploaded object keeps its name.
bool message_attachment_drafts_rename(MessageAttachmentDraftStore* store, u64 id, Utf8View filename) noexcept;
bool message_attachment_drafts_mark_queued(MessageAttachmentDraftStore* store, ConstSlice<u64> ids, u64 nonce) noexcept;
u32 message_attachment_drafts_remove_nonce(MessageAttachmentDraftStore* store, u64 nonce) noexcept;
bool message_attachment_drafts_reference(const MessageAttachmentDraftStore* store, u64 id, u32 attachment_id,
    UploadedAttachment* output) noexcept;
bool message_attachment_drafts_remove(MessageAttachmentDraftStore* store, u64 id) noexcept;
u32 message_attachment_drafts_reserved_bytes() noexcept;
}

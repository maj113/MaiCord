#include "maicord/model/message_rich_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool unchanged_tail(const u8* text, u32 count, u8 value) noexcept {
    for (u32 index = 0; index < count; ++index)
        if (index != 0 && index != count / 2 && text[index] != value)
            return false;
    return true;
}
GatewayMessageEvent sparse_message() noexcept {
    GatewayMessageEvent event{};
    event.id = 900;
    event.channel_id = 7;
    event.fields = gateway_message_field_attachments | gateway_message_field_message_snapshots;
    return event;
}
} // namespace

void test_message_rich_store_sparse(void (*check)(bool, const char*) noexcept) noexcept {
    StableStorage records{};
    MessageRichStore store{};
    u16 order[2]{7, 9};
    u8 text[256];
    for (auto& byte : text)
        byte = 0xA5;
    check(
        message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 2}, {text, sizeof(text)}) &&
            store.sparse_text && store.text_capacity == sizeof(text) && store.capacity == 2 &&
            store.record_capacity == 4 && store.record_extent == 0 && records.capacity == 0 &&
            records.arena.committed == 0 && order[0] == 0 && order[1] == 0 && text[0] == 0 &&
            text[128] == 0 && unchanged_tail(text, sizeof(text), 0xA5),
        "sparse rich owned initialization records real capacity and initializes order and only both bank "
        "sentinels");
    const auto* reserved_records = records.arena.base;
    auto event = sparse_message();
    GatewayMessageAttachment attachment{};
    attachment.id = 901;
    attachment.filename = utf8_literal("file.png");
    attachment.url = utf8_literal("https://example.com/file.png");
    attachment.size = 64;
    event.attachments = {&attachment, 1};
    event.snapshot.present = true;
    event.snapshot.content = utf8_literal("child text");
    event.snapshot.fields = gateway_message_field_content;
    check(message_rich_store_apply(&store, 900, 1, event).ok() && store.record_extent >= 2 &&
              store.snapshot_count == 1 && records.arena.base == reserved_records &&
              message_rich_store_find(store, 900) && message_rich_store_snapshot(store, 900),
          "sparse rich owned records still grow lazily and preserve independent forwarded child ownership");
    for (auto& byte : text)
        byte = 0xA5;
    check(message_rich_store_reset(&store) && store.sparse_text && store.text_capacity == sizeof(text) &&
              !store.count && !store.snapshot_count && !store.text_used && store.active_bank == 0 &&
              text[0] == 0 && text[128] == 0 && unchanged_tail(text, sizeof(text), 0xA5),
          "sparse rich reset preserves reserved capacity and leaves unused text tails untouched");
    message_rich_store_release(&store);
    check(!store.records && !store.text && !store.record_storage && !records.arena.base &&
              !records.capacity && unchanged_tail(text, sizeof(text), 0xA5),
          "sparse rich release frees owned records and clears owner without touching caller text "
          "reservation");

    for (auto& byte : text)
        byte = 0xA5;
    check(message_rich_store_initialize_owned(&store, &records, 2, {order, 2}, {text, sizeof(text)}) &&
              !store.sparse_text,
          "ordinary owned rich initializer retains its existing full-clear behavior");
    bool all_zero = true;
    for (const auto byte : text)
        all_zero &= byte == 0;
    check(all_zero, "ordinary owned rich initialization still clears the entire supplied text slice");
    for (auto& byte : text)
        byte = 0xA5;
    check(message_rich_store_reset(&store) && !store.sparse_text,
          "ordinary rich reset retains full-clear mode");
    all_zero = true;
    for (const auto byte : text)
        all_zero &= byte == 0;
    check(all_zero, "ordinary rich reset still clears both complete caller text banks");
    message_rich_store_release(&store);

    store.text_capacity = 19;
    order[0] = 7;
    order[1] = 9;
    for (auto& byte : text)
        byte = 0xA5;
    check(
        !message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 2}, {text, 255}) &&
            !records.arena.base && store.text_capacity == 19 && order[0] == 7 && order[1] == 9 &&
            text[0] == 0xA5 && text[127] == 0xA5 && text[128] == 0xA5,
        "invalid sparse text capacity rejects before modifying owner order sentinels or reserving records");
    check(!message_rich_store_initialize_owned_sparse(&store, &records, 0, {order, 2},
                                                      {text, sizeof(text)}) &&
              !message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 1},
                                                          {text, sizeof(text)}) &&
              !message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 2},
                                                          {nullptr, sizeof(text)}) &&
              !records.arena.base && store.text_capacity == 19,
          "invalid sparse record order and null text inputs leave the existing owner intact");
    check(stable_storage_initialize(&records, 2, 0, sizeof(MessageRichRecord), alignof(MessageRichRecord)),
          "sparse initialization rejection fixture owns an existing reservation");
    const auto* existing_records = records.arena.base;
    check(!message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 2},
                                                      {text, sizeof(text)}) &&
              records.arena.base == existing_records && store.text_capacity == 19,
          "sparse rich initialization does not release or replace another live record reservation");
    stable_storage_release(&records);
    store = {};

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    Arena sparse_text{};
    const bool reserved = page && arena_initialize(&sparse_text, static_cast<usize>(page) * 8, 0, page);
    const u32 bank_bytes = page * 4;
    const bool prefixes =
        reserved && VirtualAlloc(sparse_text.base, page, MEM_COMMIT, PAGE_READWRITE) == sparse_text.base &&
        VirtualAlloc(sparse_text.base + bank_bytes, page, MEM_COMMIT, PAGE_READWRITE) ==
            sparse_text.base + bank_bytes;
    check(prefixes,
          "sparse rich VM fixture commits one guarded prefix page per bank and reserves both unused tails");
    if (prefixes) {
        sparse_text.base[0] = sparse_text.base[bank_bytes] = 0xA5;
        sparse_text.base[page - 1] = sparse_text.base[bank_bytes + page - 1] = 0xA5;
        MEMORY_BASIC_INFORMATION before{}, after{}, second_tail{};
        const bool guarded =
            VirtualQuery(sparse_text.base + page, &before, sizeof(before)) == sizeof(before) &&
            before.State == MEM_RESERVE &&
            VirtualQuery(sparse_text.base + bank_bytes + page, &second_tail, sizeof(second_tail)) ==
                sizeof(second_tail) &&
            second_tail.State == MEM_RESERVE;
        check(guarded &&
                  message_rich_store_initialize_owned_sparse(&store, &records, 2, {order, 2},
                                                             {sparse_text.base, bank_bytes * 2}) &&
                  store.text_capacity == bank_bytes * 2 && store.sparse_text && sparse_text.base[0] == 0 &&
                  sparse_text.base[bank_bytes] == 0 && sparse_text.base[page - 1] == 0xA5 &&
                  sparse_text.base[bank_bytes + page - 1] == 0xA5 &&
                  VirtualQuery(sparse_text.base + page, &after, sizeof(after)) == sizeof(after) &&
                  after.State == MEM_RESERVE &&
                  VirtualQuery(sparse_text.base + bank_bytes + page, &second_tail, sizeof(second_tail)) ==
                      sizeof(second_tail) &&
                  second_tail.State == MEM_RESERVE,
              "sparse rich initialization physically leaves uncommitted text tails untouched without "
              "committing them");
        event = sparse_message();
        event.attachments = {&attachment, 1};
        check(message_rich_store_apply(&store, 900, 1, event).ok() && store.text_used < page &&
                  message_rich_store_reset(&store) && store.sparse_text &&
                  store.text_capacity == bank_bytes * 2 &&
                  VirtualQuery(sparse_text.base + page, &after, sizeof(after)) == sizeof(after) &&
                  after.State == MEM_RESERVE &&
                  VirtualQuery(sparse_text.base + bank_bytes + page, &second_tail, sizeof(second_tail)) ==
                      sizeof(second_tail) &&
                  second_tail.State == MEM_RESERVE,
              "sparse rich apply and reset operate on committed prefixes while both bank tails remain "
              "inaccessible");
        message_rich_store_release(&store);
        check(!records.arena.base && sparse_text.base[page - 1] == 0xA5,
              "sparse rich owned cleanup preserves independently owned physical text reservation");
    }
    message_rich_store_release(&store);
    stable_storage_release(&records);
    arena_release(&sparse_text);
    u8 audio_text[512]{};
    u16 audio_order[2]{};
    StableStorage audio_records{};
    MessageRichStore audio_store{};
    check(message_rich_store_initialize_owned(&audio_store, &audio_records, 2, {audio_order, 2},
                                              {audio_text, sizeof(audio_text)}),
          "audio rich store reserves records without per-attachment sample banks");
    char waveform[]{'A', 'P', '+', 'A'};
    attachment.filename = utf8_literal("Voice.OGG");
    attachment.content_type = utf8_literal("audio/ogg");
    attachment.waveform = {waveform, sizeof(waveform)};
    attachment.duration_ms = 1234;
    attachment.has_duration = true;
    event = {};
    event.id = 900;
    event.fields = gateway_message_field_attachments | gateway_message_field_flags;
    event.flags = kGatewayMessageVoiceFlag;
    event.attachments = {&attachment, 1};
    check(message_rich_store_apply(&audio_store, 900, 1, event).ok(),
          "audio rich store admits validated duration and waveform");
    for (auto& byte : waveform)
        byte = '!';
    const auto* audio_row = message_rich_store_find(audio_store, 900);
    check(audio_row && message_rich_record_voice(*audio_row) &&
              audio_row->attachments[0].duration_ms == 1234 && (audio_row->attachments[0].state & 8u) &&
              utf8_equal(message_rich_store_string(audio_store, audio_row->attachments[0].waveform),
                         utf8_literal("AP+A")) &&
              message_rich_attachment_audio_supported(audio_store, audio_row->attachments[0]),
          "audio rich waveform owns copied text after Gateway scratch reuse and classifies supported Ogg "
          "only");
    attachment.waveform = utf8_literal("AP+A");
    event.fields = gateway_message_field_attachments;
    event.flags = 0;
    for (u32 revision = 2; revision < 18; ++revision)
        check(message_rich_store_apply(&audio_store, 900, revision, event).ok(),
              "audio repeated attachment replacement survives packed bank compaction");
    audio_row = message_rich_store_find(audio_store, 900);
    check(audio_store.compactions && audio_row && message_rich_record_voice(*audio_row) &&
              utf8_equal(message_rich_store_string(audio_store, audio_row->attachments[0].waveform),
                         utf8_literal("AP+A")),
          "audio waveform participates in packed string relocation during actual rich bank compaction");
    const u32 used_before = audio_store.text_used, updated_before = audio_store.updated;
    const auto retained = audio_row ? audio_row->attachments[0] : MessageRichAttachment{};
    attachment.waveform = utf8_literal("TR==");
    check(!message_rich_store_apply(&audio_store, 900, 18, event).ok() &&
              audio_store.text_used == used_before && audio_store.updated == updated_before &&
              message_rich_store_find(audio_store, 900) &&
              bytes_equal(&message_rich_store_find(audio_store, 900)->attachments[0], &retained,
                          sizeof(retained)),
          "audio malformed waveform cannot partially replace retained duration or owning rich strings");
    attachment.waveform = {};
    attachment.has_duration = false;
    check(!message_rich_store_validate_attachments({&attachment, 1}),
          "audio synthetic attachment rejects nonzero duration without its known flag");
    attachment.duration_ms = 0;
    event.fields |= gateway_message_field_flags;
    attachment.filename = utf8_literal("track.mp3");
    attachment.content_type = utf8_literal("audio/mpeg");
    check(message_rich_store_apply(&audio_store, 900, 18, event).ok() &&
              message_rich_attachment_audio_supported(
                  audio_store, message_rich_store_find(audio_store, 900)->attachments[0]) &&
              message_rich_attachment_audio_format(
                  audio_store, message_rich_store_find(audio_store, 900)->attachments[0]) ==
                  MediaAttachmentFormat::mp3 &&
              !message_rich_store_find(audio_store, 900)->attachments[0].waveform.bytes &&
              !(message_rich_store_find(audio_store, 900)->attachments[0].state & 8u) &&
              !message_rich_record_voice(*message_rich_store_find(audio_store, 900)),
          "audio MP3 selects its standalone decoder and replacement clears old voice metadata");
    attachment.filename = utf8_literal("track.wav");
    attachment.content_type = utf8_literal("audio/wav");
    check(message_rich_store_apply(&audio_store, 900, 19, event).ok() &&
              message_rich_attachment_audio_format(
                  audio_store, message_rich_store_find(audio_store, 900)->attachments[0]) ==
                  MediaAttachmentFormat::wav,
          "audio WAV selects its own container admission rather than Ogg");
    attachment.filename = utf8_literal("track.OPUS");
    attachment.content_type = {};
    check(message_rich_store_apply(&audio_store, 900, 20, event).ok() &&
              message_rich_attachment_audio_supported(
                  audio_store, message_rich_store_find(audio_store, 900)->attachments[0]),
          "audio supported Opus suffix remains eligible when optional content type is absent");
    const auto classified = [&](Utf8View filename, Utf8View type, u32 revision,
                                MediaAttachmentFormat expected) noexcept {
        attachment.filename = filename;
        attachment.content_type = type;
        return message_rich_store_apply(&audio_store, 900, revision, event).ok() &&
               message_rich_attachment_audio_format(
                   audio_store, message_rich_store_find(audio_store, 900)->attachments[0]) == expected;
    };
    check(classified(utf8_literal("download.bin"), utf8_literal("AUDIO/MP3; codecs=mp3"), 21,
                     MediaAttachmentFormat::mp3),
          "audio typed MP3 MIME is case-insensitive and parameters preserve the owning label");
    check(classified(utf8_literal("original.MP3"), utf8_literal("application/octet-stream"), 22,
                     MediaAttachmentFormat::mp3),
          "audio generic MIME permits an exact MP3 suffix without losing codec identity");
    check(classified(utf8_literal("download.bin"), utf8_literal("audio/vnd.wave"), 23,
                     MediaAttachmentFormat::wav) &&
              classified(utf8_literal("download.bin"), utf8_literal("audio/x-wav"), 24,
                         MediaAttachmentFormat::wav),
          "audio standard WAVE MIME aliases select the PCM container path");
    check(classified(utf8_literal("original.WAV"), {}, 25, MediaAttachmentFormat::wav),
          "audio optional missing MIME admits the exact WAV suffix");
    check(classified(utf8_literal("original.wav"), utf8_literal("audio/flac"), 26,
                     MediaAttachmentFormat::unknown),
          "audio known unsupported MIME cannot become WAV through a conflicting filename");
    check(classified(utf8_literal("original.mp3.exe"), utf8_literal("application/octet-stream"), 27,
                     MediaAttachmentFormat::unknown),
          "audio filename admission matches its complete suffix rather than a contained extension");
    check(classified(utf8_literal("memo.bin"), utf8_literal("audio/mp4"), 28, MediaAttachmentFormat::m4a) &&
              classified(utf8_literal("memo.bin"), utf8_literal("audio/x-m4a"), 29, MediaAttachmentFormat::m4a) &&
              classified(utf8_literal("Voice Memo.M4A"), {}, 30, MediaAttachmentFormat::m4a) &&
              classified(utf8_literal("clip.m4a"), utf8_literal("video/mp4"), 31, MediaAttachmentFormat::unknown),
          "audio MPEG-4 AAC MIME and generic .m4a suffix select the MP4 audio container, never a video type");
    check(message_rich_store_reset(&audio_store) && !audio_store.count && !audio_store.text_used,
          "audio account reset releases waveform logical ownership along with other rich metadata");
    message_rich_store_release(&audio_store);
}
} // namespace maicord

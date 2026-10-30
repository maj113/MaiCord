#include "maicord/model/message_rich_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/media/asset_key.hpp"

#include "maicord/core/win32_kernel.hpp"

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

// Reaction mutations edit the resident record in place: counts, ownership,
// membership order, capacity and text-bank pressure.
void check_reaction_mutations(void (*check)(bool, const char*) noexcept) noexcept {
    static MessageRichRecord records[4];
    static u16 order[4];
    static u8 text[192];
    MessageRichStore store{};
    if (!message_rich_store_initialize(&store, {records, 4}, {order, 4}, {text, sizeof(text)})) {
        check(false, "rich reactions: the store initializes");
        return;
    }
    constexpr Utf8View party = utf8_literal("\xF0\x9F\x8E\x89");
    constexpr Utf8View heart = utf8_literal("\xE2\x9D\xA4");
    using Kind = MessageRichReactionMutationKind;
    const auto mutate = [&](u32 revision, u64 emoji, Utf8View name, MessageReactionType type, Kind kind, bool me,
                            bool animated = false) noexcept {
        return message_rich_store_mutate_reaction(&store, 70, revision, {emoji, name, type, kind, me, animated});
    };
    const auto normal = MessageReactionType::normal;
    const auto burst = MessageReactionType::burst;

    auto result = mutate(1, 0, party, normal, Kind::add, true);
    const MessageRichRecord* rich = message_rich_store_find(store, 70);
    check(result.ok() && result.inserted && rich && rich->reaction_count == 1 && rich->reactions[0].count == 1 &&
              rich->reactions[0].normal_count == 1 && (rich->reactions[0].flags & 1u) &&
              message_rich_store_string(store, rich->reactions[0].name).bytes == party.bytes,
          "rich reactions: the first reaction creates the record owned by the current user");

    const u32 updated = store.updated;
    result = mutate(1, 0, party, normal, Kind::add, true);
    check(result.ok() && result.record && store.updated == updated && rich->reactions[0].count == 1,
          "rich reactions: adding an owned reaction again changes nothing");

    result = mutate(1, 44, utf8_literal("party"), burst, Kind::add, false, true);
    check(result.ok() && rich->reaction_count == 2 && rich->reactions[1].emoji_id == 44 &&
              rich->reactions[1].burst_count == 1 && rich->reactions[1].count == 1 &&
              (rich->reactions[1].flags & 4u) && !(rich->reactions[1].flags & 3u),
          "rich reactions: a burst by another user adds an animated custom entry");

    result = mutate(1, 0, party, normal, Kind::add, false);
    check(result.ok() && rich->reaction_count == 2 && rich->reactions[0].count == 2 &&
              rich->reactions[0].normal_count == 2 && (rich->reactions[0].flags & 1u),
          "rich reactions: another user's reaction joins the same Unicode entry");

    result = mutate(1, 0, heart, normal, Kind::add, false);
    check(result.ok() && rich->reaction_count == 3 && rich->reactions[2].count == 1,
          "rich reactions: a different Unicode name is a separate entry");

    result = mutate(1, 0, party, normal, Kind::remove, true);
    check(result.ok() && rich->reactions[0].count == 1 && rich->reactions[0].normal_count == 1 &&
              !(rich->reactions[0].flags & 1u),
          "rich reactions: removing the own reaction clears ownership only");
    result = mutate(1, 0, party, normal, Kind::remove, true);
    check(result.ok() && rich->reactions[0].count == 1,
          "rich reactions: removing an unowned own reaction changes nothing");

    const u32 wasted = store.text_wasted;
    result = mutate(1, 0, party, normal, Kind::remove, false);
    check(result.ok() && rich->reaction_count == 2 && rich->reactions[0].emoji_id == 44 &&
              rich->reactions[1].emoji_id == 0 && store.text_wasted >= wasted + party.bytes,
          "rich reactions: the last removal drops the entry, keeps the others in order and wastes its name");

    const u32 rejected = store.rejected;
    result = mutate(9, 44, utf8_literal("party"), burst, Kind::add, false);
    check(!result.ok() && store.rejected == rejected + 1 && rich->reactions[0].count == 1,
          "rich reactions: a stale revision is rejected");

    result = mutate(1, 44, {}, burst, Kind::remove_emoji, false);
    check(result.ok() && rich->reaction_count == 1 && rich->reactions[0].emoji_id == 0,
          "rich reactions: removing an emoji drops its entry by snowflake");
    result = mutate(1, 0, {}, normal, Kind::remove_all, false);
    check(result.ok() && !result.record && !message_rich_store_find(store, 70) && !store.count,
          "rich reactions: removing every reaction frees a record that holds nothing else");
    result = mutate(1, 0, party, normal, Kind::remove, false);
    check(result.ok() && !result.record, "rich reactions: removing from a message without a record is a no-op");

    // The record keeps at most kGatewayMessageReactionMaximum entries.
    bool filled = true;
    for (u32 id = 1; id <= kGatewayMessageReactionMaximum; ++id)
        filled = filled && mutate(1, id, utf8_literal("e"), normal, Kind::add, false).ok();
    result = mutate(1, 99, utf8_literal("e"), normal, Kind::add, false);
    rich = message_rich_store_find(store, 70);
    bool grown_intact = rich != nullptr;
    for (u32 id = 1; grown_intact && id <= kGatewayMessageReactionMaximum; ++id)
        grown_intact = rich->reactions[id - 1].emoji_id == id && rich->reactions[id - 1].count == 1;
    check(filled && rich && rich->reaction_count == kGatewayMessageReactionMaximum &&
              result.error == MessageRichStoreError::record_capacity && grown_intact,
          "rich reactions: a full record rejects another entry and keeps its entries across the class growth");
    mutate(1, 0, {}, normal, Kind::remove_all, false);

    // Names churn through a 96-byte bank: compaction keeps every live name intact.
    bool intact = mutate(1, 0, party, normal, Kind::add, false).ok();
    for (u32 round = 0; round < 40 && intact; ++round) {
        const char name[] = {'n', static_cast<char>('a' + round % 26), 'm', 'e', '_', '1', '2', '3'};
        const Utf8View view{name, static_cast<u32>(sizeof(name))};
        intact = mutate(1, 0, view, normal, Kind::add, false).ok();
        rich = message_rich_store_find(store, 70);
        const Utf8View kept = rich ? message_rich_store_string(store, rich->reactions[1].name) : Utf8View{};
        intact = intact && rich && rich->reaction_count == 2 && kept.bytes == view.bytes &&
                 bytes_equal(kept.data, name, view.bytes) &&
                 message_rich_store_string(store, rich->reactions[0].name).bytes == party.bytes &&
                 mutate(1, 0, view, normal, Kind::remove, false).ok();
    }
    check(intact && store.compactions != 0 && !(rich && (rich->truncations & gateway_message_rich_truncated_text)),
          "rich reactions: text-bank pressure compacts and keeps live names");
}
} // namespace

// Item arrays live in recycled fixed-size side blocks: a record takes the smallest class that fits,
// returns the block when the array empties or the record goes, and nothing leaks across churn.
void check_side_blocks(void (*check)(bool, const char*) noexcept) noexcept {
    static MessageRichRecord records[4];
    static u16 order[4];
    static u8 text[2048];
    MessageRichStore store{};
    if (!message_rich_store_initialize(&store, {records, 4}, {order, 4}, {text, sizeof(text)})) {
        check(false, "rich side blocks: the store initializes");
        return;
    }
    GatewayMessageAttachment attachments[3]{};
    for (u32 index = 0; index < 3; ++index) {
        attachments[index].id = 10 + index;
        attachments[index].filename = utf8_literal("a.bin");
        attachments[index].url = utf8_literal("https://example.com/a.bin");
    }
    GatewayMessageEvent event{};
    event.id = 70;
    event.fields = gateway_message_field_attachments;
    const auto apply = [&](u32 revision, u32 count) noexcept {
        event.attachments = {attachments, count};
        return message_rich_store_apply(&store, 70, revision, event);
    };
    constexpr u32 kAttachmentKind = 1; // MessageRichSide slab row of the attachment array
    const MessageRichSlab& small = store.side.slabs[kAttachmentKind][0];
    const MessageRichSlab& large = store.side.slabs[kAttachmentKind][1];

    auto result = apply(1, 2);
    const MessageRichRecord* rich = message_rich_store_find(store, 70);
    check(result.ok() && rich && rich->attachment_count == 2 && rich->attachments[1].id == 11 && small.top == 1 &&
              large.top == 0,
          "rich side blocks: two attachments take one small block");
    const MessageRichAttachment* const small_block = rich->attachments;
    result = apply(2, 1);
    check(result.ok() && rich->attachment_count == 1 && rich->attachments == small_block && small.top == 1,
          "rich side blocks: a smaller replacement reuses the block in place");
    result = apply(3, 3);
    check(result.ok() && rich->attachment_count == 3 && rich->attachments[2].id == 12 && rich->attachments != small_block &&
              large.top == 1 && small.free_head != 0,
          "rich side blocks: a third attachment moves to the large class and frees the small block");
    result = apply(4, 0);
    check(result.ok() && !result.record && !message_rich_store_find(store, 70) && large.free_head != 0,
          "rich side blocks: emptying the array frees its block with the record");
    bool churn = true;
    for (u32 round = 0; round < 12 && churn; ++round)
        churn = apply(5 + 2 * round, 2).ok() && message_rich_store_delete(&store, 70);
    check(churn && small.top == 1 && large.top == 1,
          "rich side blocks: apply and delete churn recycles blocks without growing the slabs");
    check(apply(40, 2).ok() && message_rich_store_reset(&store) && !message_rich_store_find(store, 70) &&
              small.top == 0 && large.top == 0,
          "rich side blocks: reset returns every block");
    message_rich_store_release(&store);
    check(!store.side.initialized, "rich side blocks: release frees the slabs");
}

void test_message_rich_store_sparse(void (*check)(bool, const char*) noexcept) noexcept {
    check_reaction_mutations(check);
    check_side_blocks(check);
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
    check(message_rich_store_initialize_owned_sparse(&audio_store, &audio_records, 2, {audio_order, 2},
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

    // A record slot that is not in use is all zero except its (nonzero)
    // generation: insert and unpack rely on it instead of clearing the slot.
    static MessageRichRecord free_records[4];
    static u16 free_order[4];
    static u8 free_text[8192];
    u8* const raw_records = reinterpret_cast<u8*>(free_records);
    for (u32 index = 0; index < sizeof(free_records); ++index)
        raw_records[index] = 0xA5;
    const auto free_slots_clean = [](const MessageRichStore& owner) noexcept {
        MessageRichRecord expected;
        for (u32 index = 0; index < owner.record_extent; ++index) {
            const MessageRichRecord& slot = owner.records[index];
            if (slot.occupied)
                continue;
            message_rich_record_clear(&expected);
            expected.generation = slot.generation;
            if (slot.generation == 0 || !bytes_equal(&slot, &expected, sizeof(slot)))
                return false;
        }
        return true;
    };
    MessageRichStore free_store{};
    check(message_rich_store_initialize(&free_store, {free_records, 4}, {free_order, 4}, {free_text, sizeof(free_text)}) &&
              free_store.record_extent == 4 && free_slots_clean(free_store),
          "rich free slots: initialization clears every slot to its generation");
    auto forwarded = sparse_message();
    GatewayMessageAttachment forwarded_attachment{};
    forwarded_attachment.id = 911;
    forwarded_attachment.filename = utf8_literal("a.png");
    forwarded_attachment.url = utf8_literal("https://example.com/a.png");
    forwarded_attachment.size = 8;
    forwarded.attachments = {&forwarded_attachment, 1};
    forwarded.snapshot.present = true;
    forwarded.snapshot.content = utf8_literal("quoted");
    forwarded.snapshot.fields = gateway_message_field_content;
    u8 packed[4096];
    u32 packed_bytes = 0;
    check(message_rich_store_apply(&free_store, 900, 1, forwarded).ok() &&
              message_rich_store_snapshot(free_store, 900) && free_slots_clean(free_store) &&
              message_rich_store_pack(free_store, 900, {packed, sizeof(packed)}, &packed_bytes) && packed_bytes &&
              message_rich_store_delete(&free_store, 900) && free_slots_clean(free_store),
          "rich free slots: a deleted forwarded message leaves both slots clean");
    check(message_rich_store_unpack(&free_store, 900, 2, {packed, packed_bytes}) &&
              message_rich_store_snapshot(free_store, 900) && free_slots_clean(free_store) &&
              message_rich_store_delete(&free_store, 900) && free_slots_clean(free_store),
          "rich free slots: unpack writes a clean free slot and deleting it cleans it again");
    bool corrupt_clean = true;
    for (u32 at = 0; at < packed_bytes && corrupt_clean; ++at) {
        const u8 original = packed[at];
        packed[at] = 0xFF;
        const bool admitted = message_rich_store_unpack(&free_store, 900, 3, {packed, packed_bytes});
        packed[at] = original;
        corrupt_clean = (!admitted || message_rich_store_delete(&free_store, 900)) && free_slots_clean(free_store) &&
                        !free_store.count && !free_store.snapshot_count;
    }
    check(corrupt_clean, "rich free slots: a corrupted packed message is rejected or removed with every slot clean");
}
} // namespace maicord

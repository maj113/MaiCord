#include "../../support/test_main_shared.hpp"

#include "maicord/model/guild/member_store.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/app/media/message_media_controller.hpp"
#include "maicord/model/chat/message_draft_store.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {

MessageRichRecord g_message_rich_records[4];
u16 g_message_rich_order[4];
u8 g_message_rich_text[4 * 1024];

} // namespace

void test_user_store() noexcept {
    UserStore store{};
    UserRecord records[2]{};
    u32 order[2]{};
    u8 names[64]{};
    const bool initialized = user_store_initialize(&store, Slice<UserRecord>{records, 2},
                                                           Slice<u32>{order, 2}, Slice<u8>{names, 64});
    const UserStoreResult alice = user_store_upsert(&store, 1, utf8_literal("Alice"));
    const UserStoreResult bob = user_store_upsert(&store, 2, utf8_literal("Bob"));
    const UserStoreResult touch = user_store_upsert(&store, 1, utf8_literal("Alice"));
    const UserStoreResult cara = user_store_upsert(&store, 3, utf8_literal("Cara"));
    expect(initialized && alice.ok() && alice.inserted && bob.ok() && touch.ok() && !touch.name_changed &&
               cara.ok() && cara.evicted && store.count == 2 && store.evictions == 1 &&
               user_store_find(store, 1) && !user_store_find(store, 2) &&
               user_store_find(store, 3),
           "user_store_lru_eviction");

    const UserStoreResult renamed_alice =
        user_store_upsert(&store, 1, utf8_literal("AliceLongNam"));
    const UserStoreResult renamed_cara =
        user_store_upsert(&store, 3, utf8_literal("CaraLongName"));
    const UserRecord* renamed = user_store_find(store, 3);
    expect(renamed_alice.ok() && renamed_cara.ok() && renamed_cara.name_changed && store.compactions == 1 &&
               renamed &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(
                                              user_store_name(store, *renamed).data),
                                          user_store_name(store, *renamed).bytes},
                           byte_literal("CaraLongName")),
           "user_store_name_compaction");

    constexpr Utf8View avatar_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const bool avatar_set = user_store_set_avatar(&store, 3, avatar_hash);
    renamed = user_store_find(store, 3);
    const Utf8View stored_avatar = renamed ? user_store_avatar_hash(*renamed) : Utf8View{};
    expect(avatar_set && renamed &&
               bytes_equal(
                   ConstSlice<u8>{reinterpret_cast<const u8*>(stored_avatar.data), stored_avatar.bytes},
                   byte_literal("0123456789abcdef0123456789abcdef")) &&
               user_avatar_variant(stored_avatar) != 0 &&
               !user_store_set_avatar(&store, 3, utf8_literal("../../invalid")),
           "user_store_avatar_hash_and_variant");

    const char invalid[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    expect(user_store_upsert(&store, 4, Utf8View{invalid, 2}).error ==
                   UserStoreError::invalid_utf8 &&
               user_store_upsert(&store, 0, utf8_literal("none")).error ==
                   UserStoreError::invalid_input,
           "user_store_validation");

    UserStore paired{};
    UserRecord paired_records[2]{};
    u32 paired_order[2]{};
    u8 paired_names[64]{};
    const bool paired_ready =
        user_store_initialize(&paired, {paired_records, 2}, {paired_order, 2}, {paired_names, 64}) &&
        user_store_upsert(&paired, 1, utf8_literal("Alice"), utf8_literal("alice")).ok() &&
        user_store_upsert(&paired, 2, utf8_literal("Bob"), utf8_literal("bob")).ok();
    const u32 paired_revision = paired.revision;
    const u32 paired_clock = paired.clock;
    const u32 paired_used = paired.name_used;
    const UserStoreResult paired_touch =
        user_store_upsert(&paired, 1, utf8_literal("Alice"), utf8_literal("alice"));
    const UserStoreResult preserved_touch =
        user_store_upsert(&paired, 1, utf8_literal("Alice"));
    const UserRecord* touched = user_store_find(paired, 1);
    expect(paired_ready && paired_touch.ok() && !paired_touch.name_changed &&
               preserved_touch.ok() && !preserved_touch.name_changed &&
               paired.revision == paired_revision && paired.name_used == paired_used &&
               paired.clock == paired_clock + 2 && touched && touched->last_seen == paired.clock &&
               utf8_equal(user_store_username(paired, *touched), utf8_literal("alice")),
           "user_store_unchanged_name_pair_preserves_lru_and_storage");

    const bool paired_renamed =
        user_store_upsert(&paired, 1, utf8_literal("Alice2")).ok();
    const UserRecord* borrowed_record = user_store_find(paired, 2);
    const Utf8View borrowed_display =
        borrowed_record ? user_store_name(paired, *borrowed_record) : Utf8View{};
    const UserStoreResult borrowed_update =
        user_store_upsert(&paired, 1, borrowed_display);
    touched = user_store_find(paired, 1);
    expect(paired_renamed && borrowed_update.ok() && borrowed_update.name_changed &&
               paired.compactions == 1 && touched &&
               utf8_equal(user_store_name(paired, *touched), utf8_literal("Bob")) &&
               utf8_equal(user_store_username(paired, *touched), utf8_literal("alice")),
           "user_store_borrowed_names_survive_compaction");

    const UserStoreResult repartitioned =
        user_store_upsert(&paired, 1, utf8_literal("Boba"), utf8_literal("lice"));
    touched = user_store_find(paired, 1);
    expect(repartitioned.ok() && repartitioned.name_changed && touched &&
               utf8_equal(user_store_name(paired, *touched), utf8_literal("Boba")) &&
               utf8_equal(user_store_username(paired, *touched), utf8_literal("lice")),
           "user_store_equal_concatenation_retains_name_boundaries");
}

void test_message_store() noexcept {
    MessageRecord records[4]{};
    u32 order[4]{};
    u8 text[64]{};
    MessageStore store{};
    expect(message_store_initialize(&store, Slice<MessageRecord>{records, 4}, Slice<u32>{order, 4},
                                    Slice<u8>{text, 64}),
           "message_store_initialize");
    const MessageInput third{300, 7, 13, 3'000, 0, utf8_literal("three"), 0};
    const MessageInput first{100, 7, 11, 1'000, 0, utf8_literal("one"), 1};
    const MessageInput second{200, 7, 12, 2'000, 0, utf8_literal("two"), 2};
    const MessageStoreResult inserted_third = message_store_upsert(&store, third);
    const MessageStoreResult inserted_first = message_store_upsert(&store, first);
    const MessageStoreResult inserted_second = message_store_upsert(&store, second);
    const MessageRecord* ordered0 = message_store_resolve(store, message_store_ordered_handle(store, 0));
    const MessageRecord* ordered1 = message_store_resolve(store, message_store_ordered_handle(store, 1));
    const MessageRecord* ordered2 = message_store_resolve(store, message_store_ordered_handle(store, 2));
    expect(inserted_third.ok() && inserted_first.ok() && inserted_second.ok() && inserted_third.inserted &&
               store.count == 3 && store.created == 3 && ordered0 && ordered0->id == 100 && ordered1 &&
               ordered1->id == 200 && ordered2 && ordered2->id == 300,
           "message_store_sorted_stable_order");
    expect(message_store_set_presentation_flags(&store, 200, message_presentation_mentions_current_user) &&
               message_store_resolve(store, message_store_find(store, 200).handle)->presentation_flags ==
                   message_presentation_mentions_current_user &&
               message_store_set_presentation_flags(&store, 200, message_presentation_none) &&
               !message_store_set_presentation_flags(&store, 200, 0x80u),
           "message_store_presentation_flags");

    const MessageStoreResult found_second = message_store_find(store, 200);
    const MessageInput edited{200, 7, 12, 2'000, 2'500, utf8_literal("two edited"), 6};
    const MessageStoreResult updated = message_store_upsert(&store, edited);
    const u32 used_after_update = store.text_used;
    const u32 wasted_after_update = store.text_wasted;
    const MessageStoreResult unchanged = message_store_upsert(&store, edited);
    const MessageRecord* edited_record = message_store_resolve(store, updated.handle);
    expect(found_second.ok() && updated.ok() && !updated.inserted && unchanged.ok() && edited_record &&
               edited_record->revision == 3 && edited_record->edited_timestamp_ms == 2'500 &&
               edited_record->flags == 6 &&
               utf8_equal(message_store_content(store, updated.handle), utf8_literal("two edited")) &&
               store.text_used == used_after_update && store.text_wasted == wasted_after_update &&
               store.updated == 4,
           "message_store_update_content_revision");

    const MessageHandle stale_first = inserted_first.handle;
    expect(message_store_delete(&store, 100) && !message_store_resolve(store, stale_first) &&
               store.deleted == 1,
           "message_store_delete_invalidates_handle");
    const MessageStoreResult inserted_fourth =
        message_store_upsert(&store, MessageInput{400, 7, 14, 4'000, 0, utf8_literal("four"), 0});
    expect(inserted_fourth.ok() && inserted_fourth.handle.slot == stale_first.slot &&
               inserted_fourth.handle.generation != stale_first.generation &&
               !message_store_resolve(store, stale_first),
           "message_store_slot_generation_reuse");

    expect(message_store_upsert(&store, MessageInput{500, 7, 15, 5'000, 0, utf8_literal("five"), 0}).ok() &&
               message_store_upsert(&store, MessageInput{600, 7, 16, 6'000, 0, utf8_literal("six"), 0})
                       .error == MessageStoreError::record_capacity,
           "message_store_record_capacity");
    const char malformed[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    expect(
        message_store_upsert(&store, MessageInput{700, 7, 17, 7'000, 0, Utf8View{malformed, 2}, 0}).error ==
                MessageStoreError::invalid_utf8 &&
            message_store_find(store, 999).error == MessageStoreError::not_found,
        "message_store_utf8_and_not_found");

    MessageRecord small_records[2]{};
    u32 small_order[2]{};
    u8 small_text[4]{};
    MessageStore small_store{};
    expect(
        message_store_initialize(&small_store, Slice<MessageRecord>{small_records, 2},
                                 Slice<u32>{small_order, 2}, Slice<u8>{small_text, 4}) &&
            message_store_upsert(&small_store, MessageInput{1, 1, 1, 1, 0, utf8_literal("1234"), 0}).ok() &&
            message_store_upsert(&small_store, MessageInput{1, 1, 1, 1, 0, utf8_literal("12345"), 0})
                    .error == MessageStoreError::text_capacity,
        "message_store_text_capacity_atomic");

    MessageRecord in_place_records[5]{};
    u32 in_place_order[5]{};
    u8 in_place_text[64]{};
    MessageStore in_place_store{};
    expect(
        message_store_initialize(&in_place_store, Slice<MessageRecord>{in_place_records, 5},
                                 Slice<u32>{in_place_order, 5}, Slice<u8>{in_place_text, 64}) &&
            message_store_upsert(&in_place_store, MessageInput{10, 1, 1, 1, 0, utf8_literal("aaaa"), 0})
                .ok() &&
            message_store_upsert(&in_place_store, MessageInput{20, 1, 1, 2, 0, utf8_literal(""), 0}).ok() &&
            message_store_upsert(&in_place_store, MessageInput{30, 1, 1, 3, 0, utf8_literal("cccc"), 0})
                .ok(),
        "message_store_in_place_fixture");
    const MessageHandle in_place_10 = message_store_find(in_place_store, 10).handle;
    const MessageHandle in_place_20 = message_store_find(in_place_store, 20).handle;
    expect(message_store_upsert(&in_place_store, MessageInput{10, 1, 1, 1, 4, utf8_literal("bb"), 1,
                                                              MessageContentAvailability::restricted})
                   .ok() &&
               message_store_delete(&in_place_store, 30) &&
               message_store_upsert(&in_place_store, MessageInput{40, 1, 1, 4, 0, utf8_literal("dddd"), 0})
                   .ok() &&
               in_place_store.text_used == 12 && in_place_store.text_wasted == 6 &&
               message_store_compact_in_place(&in_place_store) && in_place_store.text_used == 6 &&
               in_place_store.text_wasted == 0 && message_store_resolve(in_place_store, in_place_10) &&
               message_store_resolve(in_place_store, in_place_20) &&
               message_store_resolve(in_place_store, in_place_10)->content_availability ==
                   MessageContentAvailability::restricted &&
               utf8_equal(message_store_content(in_place_store, in_place_10), utf8_literal("bb")) &&
               message_store_resolve(in_place_store, in_place_20)->content_offset == 6 &&
               utf8_equal(
                   message_store_content(in_place_store, message_store_find(in_place_store, 40).handle),
                   utf8_literal("dddd")),
           "message_store_in_place_compaction_preserves_handles");
    const MessageInput invalid_availability{
        50, 1, 1, 5, 0, utf8_literal("x"), 0, static_cast<MessageContentAvailability>(99)};
    expect(message_store_validate_input(&in_place_store, invalid_availability) ==
                   MessageStoreError::invalid_input &&
               message_store_upsert(&in_place_store, invalid_availability).error ==
                   MessageStoreError::invalid_input,
           "message_store_content_availability_validation");

    MessageRecord pressure_records[4]{};
    u32 pressure_order[4]{};
    u8 pressure_text[8]{};
    MessageStore pressure_store{};
    expect(message_store_initialize(&pressure_store, Slice<MessageRecord>{pressure_records, 4},
                                    Slice<u32>{pressure_order, 4}, Slice<u8>{pressure_text, 8}) &&
               message_store_upsert(&pressure_store, MessageInput{1, 1, 1, 1, 0, utf8_literal("aa"), 0})
                   .ok() &&
               message_store_upsert(&pressure_store, MessageInput{2, 1, 1, 2, 0, utf8_literal("bb"), 0})
                   .ok() &&
               message_store_delete(&pressure_store, 1) &&
               message_store_upsert(&pressure_store, MessageInput{3, 1, 1, 3, 0, utf8_literal("cccccc"), 0})
                   .ok() &&
               pressure_store.text_used == 8 && pressure_store.text_wasted == 0 &&
               pressure_store.compactions == 1 &&
               utf8_equal(
                   message_store_content(pressure_store, message_store_find(pressure_store, 3).handle),
                   utf8_literal("cccccc")),
           "message_store_insert_compacts_on_tail_pressure");

    MessageRecord replace_records[3]{};
    u32 replace_order[3]{};
    u8 replace_text[8]{};
    MessageStore replace_store{};
    expect(
        message_store_initialize(&replace_store, Slice<MessageRecord>{replace_records, 3},
                                 Slice<u32>{replace_order, 3}, Slice<u8>{replace_text, 8}) &&
            message_store_upsert(&replace_store, MessageInput{1, 1, 1, 1, 0, utf8_literal("aaa"), 0})
                .ok() &&
            message_store_upsert(&replace_store, MessageInput{2, 1, 1, 2, 0, utf8_literal("bb"), 0}).ok() &&
            message_store_delete(&replace_store, 2) &&
            message_store_upsert(&replace_store, MessageInput{1, 1, 1, 1, 3, utf8_literal("123456"), 0})
                .ok() &&
            replace_store.text_used == 6 && replace_store.text_wasted == 0 &&
            replace_store.compactions == 1 &&
            utf8_equal(
                message_store_content(replace_store, message_store_find(replace_store, 1).handle),
                utf8_literal("123456")),
        "message_store_replacement_compacts_without_scratch");

    MessageRecord overlap_records[3]{};
    u32 overlap_order[3]{};
    u8 overlap_text[4]{};
    MessageStore overlap_store{};
    expect(
        message_store_initialize(&overlap_store, Slice<MessageRecord>{overlap_records, 3},
                                 Slice<u32>{overlap_order, 3}, Slice<u8>{overlap_text, 4}) &&
            message_store_upsert(&overlap_store, MessageInput{1, 1, 1, 1, 0, utf8_literal("aa"), 0}).ok() &&
            message_store_upsert(&overlap_store, MessageInput{2, 1, 1, 2, 0, utf8_literal("bb"), 0}).ok() &&
            message_store_delete(&overlap_store, 2),
        "message_store_overlap_fixture");
    const MessageStoreResult overlap_rejected = message_store_upsert(
        &overlap_store,
        MessageInput{1, 1, 1, 1, 3, Utf8View{reinterpret_cast<const char*>(overlap_store.text), 4}, 0});
    expect(overlap_rejected.error == MessageStoreError::invalid_input && overlap_store.count == 1 &&
               overlap_store.text_used == 4 && overlap_store.text_wasted == 2 &&
               utf8_equal(
                   message_store_content(overlap_store, message_store_find(overlap_store, 1).handle),
                   utf8_literal("aa")),
           "message_store_overlap_rejection_is_atomic");
    const u64 timestamp = kSnowflakeEpochMilliseconds + 123'456;
    const u64 snowflake = (timestamp - kSnowflakeEpochMilliseconds) << 22;
    expect(snowflake_timestamp_ms(snowflake) == timestamp, "message_store_snowflake_timestamp");
}

void test_message_rich_store() noexcept {
    MessageRichStore store{};
    expect(message_rich_store_initialize(&store, Slice<MessageRichRecord>{g_message_rich_records, 4},
                                         Slice<u16>{g_message_rich_order, 4},
                                         Slice<u8>{g_message_rich_text, 4 * 1024}),
           "message_rich_store_initialize");

    GatewayMessageReaction reactions[1]{};
    reactions[0].emoji_id = 44;
    reactions[0].name = utf8_literal("party");
    reactions[0].count = 4;
    reactions[0].normal_count = 3;
    reactions[0].burst_count = 1;
    reactions[0].me = true;
    reactions[0].animated = true;
    GatewayMessageAttachment attachments[1]{};
    attachments[0].id = 70;
    attachments[0].size = 12'345;
    attachments[0].filename = utf8_literal("clip.mp4");
    attachments[0].content_type = utf8_literal("video/mp4");
    attachments[0].url = utf8_literal("https://cdn.discordapp.com/attachments/7/70/clip.mp4");
    attachments[0].width = 1'280;
    attachments[0].height = 720;
    attachments[0].has_width = true;
    attachments[0].has_height = true;
    GatewayMessageEmbed embeds[1]{};
    embeds[0].title = utf8_literal("Build");
    embeds[0].description = utf8_literal("green");
    embeds[0].type = GatewayMessageEmbedType::rich;
    embeds[0].color = 0x00FF00u;
    embeds[0].has_color = true;
    embeds[0].field_count = 1;
    embeds[0].image.url = utf8_literal("https://example.com/original.png");
    embeds[0].image.proxy_url = utf8_literal("https://media.discordapp.net/external/store/image");
    embeds[0].image.description = utf8_literal("stored alt");
    embeds[0].image.width = 640;
    embeds[0].image.height = 360;
    embeds[0].image.flags = 32;
    embeds[0].image.has_width = true;
    embeds[0].image.has_height = true;
    GatewayMessageEmbedField embed_fields[1]{};
    embed_fields[0].name = utf8_literal("Target");
    embed_fields[0].value = utf8_literal("Release");
    embed_fields[0].inline_field = true;
    GatewayMessageSticker stickers[1]{};
    stickers[0].id = 88;
    stickers[0].name = utf8_literal("Wave");
    stickers[0].format_type = 2;
    GatewayMessageComponent components[2]{};
    components[0].type = 17;
    components[0].parent = 0xFFFFu;
    components[1].type = 10;
    components[1].parent = 0;
    components[1].content = utf8_literal("Component markdown");

    GatewayMessageEvent rich{};
    rich.id = 900;
    rich.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message |
                  gateway_message_field_reactions | gateway_message_field_attachments |
                  gateway_message_field_embeds | gateway_message_field_sticker_items |
                  gateway_message_field_components;
    rich.reference.message_id = 800;
    rich.reference.channel_id = 7;
    rich.reference.author_id = 12;
    rich.reference.author_name = utf8_literal("Source");
    rich.reference.content = utf8_literal("the referenced text");
    rich.reference.state = GatewayMessageReferenceState::available;
    rich.reactions = ConstSlice<GatewayMessageReaction>{reactions, 1};
    rich.attachments = ConstSlice<GatewayMessageAttachment>{attachments, 1};
    rich.embeds = ConstSlice<GatewayMessageEmbed>{embeds, 1};
    rich.embed_fields = ConstSlice<GatewayMessageEmbedField>{embed_fields, 1};
    rich.stickers = ConstSlice<GatewayMessageSticker>{stickers, 1};
    rich.components = ConstSlice<GatewayMessageComponent>{components, 2};

    StableStorage lazy_record_storage{};
    MessageRichStore lazy_store{};
    u16 lazy_order[4]{};
    u8 lazy_text[4 * 1024]{};
    const bool lazy_initialized = message_rich_store_initialize_owned_sparse(
        &lazy_store, &lazy_record_storage, 4, Slice<u16>{lazy_order, 4}, Slice<u8>{lazy_text, 4 * 1024});
    const MessageRichStoreResult lazy_inserted =
        lazy_initialized ? message_rich_store_apply(&lazy_store, 900, 1, rich) : MessageRichStoreResult{};
    const usize lazy_committed = lazy_record_storage.arena.committed;
    const bool lazy_reused = lazy_inserted.ok() && message_rich_store_delete(&lazy_store, 900) &&
                             message_rich_store_apply(&lazy_store, 900, 2, rich).ok();
    expect(lazy_initialized && lazy_store.record_extent == 1 && lazy_record_storage.capacity == 1 &&
               lazy_committed != 0 && lazy_reused &&
               message_rich_store_reset(&lazy_store) && lazy_store.count == 0 &&
               lazy_store.record_extent == 1,
           "message_rich_store_owned_records_commit_on_demand_and_reuse_slots");
    message_rich_store_release(&lazy_store);
    expect(!lazy_record_storage.arena.base && !lazy_store.records,
           "message_rich_store_owned_records_release");

    const MessageRichStoreResult inserted = message_rich_store_apply(&store, 900, 1, rich);
    const MessageRichRecord* record = message_rich_store_find(store, 900);
    expect(inserted.ok() && inserted.inserted && !inserted.storage_truncated && record &&
               record->message_revision == 1 && record->reference.message_id == 800 &&
               record->reaction_count == 1 && record->attachment_count == 1 && record->embed_count == 1 &&
               record->embed_field_count == 1 && record->sticker_count == 1 &&
               record->component_count == 2 &&
               utf8_equal(message_rich_store_string(store, record->reference.author_name),
                          utf8_literal("Source")) &&
               utf8_equal(message_rich_store_string(store, record->attachments[0].filename),
                          utf8_literal("clip.mp4")) &&
               record->embeds[0].image.width == 640 && record->embeds[0].image.height == 360 &&
               record->embeds[0].image.flags == 32 && record->embed_fields[0].inline_field &&
               utf8_equal(message_rich_store_string(store, record->embeds[0].image.proxy_url),
                          utf8_literal("https://media.discordapp.net/external/store/image")) &&
               utf8_equal(message_rich_store_string(store, record->embeds[0].image.description),
                          utf8_literal("stored alt")),
           "message_rich_store_full_insert");

    MessageRichEmbedMediaSelection selected_media{};
    float selected_width = 0.0f;
    float selected_height = 0.0f;
    expect(record && message_rich_embed_media_select(store, record->embeds[0], &selected_media) &&
               utf8_equal(selected_media.source,
                          utf8_literal("https://media.discordapp.net/external/store/image")) &&
               utf8_equal(selected_media.fallback_source,
                          utf8_literal("https://example.com/original.png")) &&
               message_rich_embed_media_extent(selected_media, 900.0f, &selected_width, &selected_height) &&
               selected_width == 400.0f && selected_height == 225.0f,
           "message_rich_embed_prefers_proxy_retains_original_and_caps_width");
    MessageRichEmbedMedia square_media{};
    if (record) {
        auto thumbnail_embed = record->embeds[0];
        thumbnail_embed.type = GatewayMessageEmbedType::rich;
        thumbnail_embed.thumbnail = thumbnail_embed.image;
        thumbnail_embed.image = {};
        MessageRichEmbedMediaSelection thumbnail_selection{};
        expect(!message_rich_embed_media_select(store, thumbnail_embed, &thumbnail_selection) &&
            message_rich_embed_thumbnail_extent(store, thumbnail_embed, 400, &selected_width, &selected_height) &&
            selected_width == 80 && selected_height == 45 &&
            message_rich_embed_resource_select(store, thumbnail_embed, MediaEmbedResource::thumbnail, &thumbnail_selection),
            "rich_thumbnail_is_separate_from_main_image_and_bounded");
        thumbnail_embed.image = record->embeds[0].image;
        expect(message_rich_embed_media_select(store, thumbnail_embed, &selected_media) &&
            selected_media.resource == MediaEmbedResource::image &&
            message_rich_embed_thumbnail_extent(store, thumbnail_embed, 400, &selected_width, &selected_height),
            "rich_embed_keeps_both_image_and_thumbnail");
    }
    square_media.width = 1'024;
    square_media.height = 1'024;
    const MessageRichEmbedMediaSelection square_selection{
        &square_media, Utf8View{}, Utf8View{}, MediaEmbedResource::image, MediaAttachmentFormat::png};
    expect(message_rich_embed_media_extent(square_selection, 900.0f, &selected_width, &selected_height) &&
               selected_width == 300.0f && selected_height == 300.0f,
           "message_rich_embed_square_is_aspect_preserving_and_height_bounded");

    GatewayMessageEmbed gifv_embeds[1]{};
    gifv_embeds[0].type = GatewayMessageEmbedType::gifv;
    gifv_embeds[0].url = utf8_literal("https://klipy.com/gifs/example");
    gifv_embeds[0].thumbnail.url =
        utf8_literal("https://static.klipy.com/poster.webp");
    gifv_embeds[0].thumbnail.proxy_url =
        utf8_literal("https://images-ext-1.discordapp.net/external/poster.webp");
    gifv_embeds[0].thumbnail.content_type = utf8_literal("image/webp");
    gifv_embeds[0].thumbnail.width = 500;
    gifv_embeds[0].thumbnail.height = 280;
    gifv_embeds[0].thumbnail.has_width = true;
    gifv_embeds[0].thumbnail.has_height = true;
    gifv_embeds[0].video.url =
        utf8_literal("https://static.klipy.com/clip.mp4");
    gifv_embeds[0].video.proxy_url =
        utf8_literal("https://images-ext-1.discordapp.net/external/clip.mp4");
    gifv_embeds[0].video.content_type = utf8_literal("video/mp4");
    gifv_embeds[0].video.width = 500;
    gifv_embeds[0].video.height = 280;
    gifv_embeds[0].video.has_width = true;
    gifv_embeds[0].video.has_height = true;
    GatewayMessageEvent gifv_event{};
    gifv_event.id = 901;
    gifv_event.fields = gateway_message_field_embeds;
    gifv_event.embeds = ConstSlice<GatewayMessageEmbed>{gifv_embeds, 1};
    const MessageRichStoreResult gifv_inserted =
        message_rich_store_apply(&store, 901, 1, gifv_event);
    const MessageRichRecord* gifv_record =
        message_rich_store_find(store, 901);
    MessageRichEmbedMediaSelection gifv_media{};
    expect(gifv_inserted.ok() && gifv_record &&
               message_rich_embed_media_only(gifv_record->embeds[0]) &&
               message_rich_embed_media_select(store, gifv_record->embeds[0],
                                               &gifv_media) &&
               gifv_media.resource == MediaEmbedResource::video &&
               gifv_media.format == MediaAttachmentFormat::mp4 &&
               gifv_media.animated &&
               utf8_equal(
                   gifv_media.source,
                   utf8_literal(
                       "https://images-ext-1.discordapp.net/external/clip.mp4")) &&
               message_rich_embed_suppresses_content(
                   store, *gifv_record,
                   utf8_literal("https://klipy.com/gifs/example")) &&
               !message_rich_embed_suppresses_content(
                   store, *gifv_record,
                   utf8_literal("text https://klipy.com/gifs/example")),
           "message_rich_gifv_selects_video_and_suppresses_exact_source_url");

    AccountMediaFrame video_frame{};
    video_frame.kind = AccountMediaFrameKind::video_nv12_gpu;
    video_frame.key = media_asset_embed(901, 1, 0, MediaEmbedResource::video,
                                        MediaAttachmentFormat::mp4, true);
    video_frame.source_message_id = 901;
    video_frame.source_message_revision = 1;
    video_frame.width = 500;
    video_frame.height = 280;
    video_frame.canvas_width = 500;
    video_frame.canvas_height = 560;
    bool video_dimensions_changed = false;
    expect(message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               video_dimensions_changed && gifv_record->embeds[0].video.width == 500 &&
               gifv_record->embeds[0].video.height == 560 &&
               gifv_record->embeds[0].thumbnail.width == gifv_embeds[0].thumbnail.width &&
               message_rich_embed_media_select(store, gifv_record->embeds[0], &gifv_media) &&
               message_rich_embed_media_extent(gifv_media, 550.0f, &selected_width, &selected_height) &&
               selected_width / selected_height > 0.892f &&
               selected_width / selected_height < 0.894f &&
               message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               !video_dimensions_changed,
           "message_video_natural_size_reflows_exact_embed_without_repeated_invalidation");
    video_frame.source_message_revision = 2;
    expect(!message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               !video_dimensions_changed && gifv_record->embeds[0].video.height == 560,
           "message_video_stale_frame_cannot_change_layout");
    video_frame.source_message_revision = 1;
    video_frame.canvas_width = 0;
    expect(!message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               !video_dimensions_changed && gifv_record->embeds[0].video.height == 560,
           "message_video_missing_display_extent_rejected");
    video_frame.canvas_width = 500;
    video_frame.key = media_asset_embed(901, 1, 1, MediaEmbedResource::video,
                                        MediaAttachmentFormat::mp4, true);
    expect(!message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               !video_dimensions_changed,
           "message_video_missing_embed_rejected");
    video_frame.key = media_asset_embed(901, 2, 0, MediaEmbedResource::video,
                                        MediaAttachmentFormat::mp4, true);
    expect(!message_media_apply_frame_dimensions(&store, video_frame, &video_dimensions_changed) &&
               !video_dimensions_changed,
           "message_video_key_revision_mismatch_rejected");

    // A decoded still refits a frame whose declared dimensions disagree with the
    // picture (square album art declared 16:9); a still within one percent of
    // the declared aspect leaves the frame alone.
    GatewayMessageEmbed music_embeds[1]{};
    music_embeds[0].type = GatewayMessageEmbedType::video;
    music_embeds[0].url = utf8_literal("https://music.youtube.com/watch?v=HC1kLPsbOLs");
    music_embeds[0].thumbnail.url = utf8_literal("https://i.ytimg.com/vi/HC1kLPsbOLs/maxresdefault.jpg");
    music_embeds[0].thumbnail.proxy_url = utf8_literal("https://images-ext-1.discordapp.net/external/poster.jpg");
    music_embeds[0].thumbnail.content_type = utf8_literal("image/jpeg");
    music_embeds[0].thumbnail.width = 1'280;
    music_embeds[0].thumbnail.height = 720;
    music_embeds[0].thumbnail.has_width = true;
    music_embeds[0].thumbnail.has_height = true;
    music_embeds[0].video.url = utf8_literal("https://www.youtube.com/embed/HC1kLPsbOLs");
    music_embeds[0].video.width = 1'280;
    music_embeds[0].video.height = 720;
    music_embeds[0].video.has_width = true;
    music_embeds[0].video.has_height = true;
    GatewayMessageEvent music_event{};
    music_event.id = 902;
    music_event.fields = gateway_message_field_embeds;
    music_event.embeds = ConstSlice<GatewayMessageEmbed>{music_embeds, 1};
    const MessageRichStoreResult music_inserted = message_rich_store_apply(&store, 902, 1, music_event);
    const MessageRichRecord* music_record = message_rich_store_find(store, 902);
    MessageRichEmbedMediaSelection music_media{};
    expect(music_inserted.ok() && music_record &&
               message_rich_embed_media_select(store, music_record->embeds[0], &music_media) &&
               music_media.resource == MediaEmbedResource::thumbnail &&
               message_rich_embed_media_extent(music_media, 900.0f, &selected_width, &selected_height) &&
               selected_width == 400.0f && selected_height == 225.0f,
           "message_embed_declared_sixteen_nine_poster_frames_wide_before_decode");
    AccountMediaFrame still_frame{};
    still_frame.kind = AccountMediaFrameKind::straight_rgba8;
    still_frame.key = media_asset_embed(902, 1, 0, MediaEmbedResource::thumbnail, MediaAttachmentFormat::jpeg);
    still_frame.source_message_id = 902;
    still_frame.source_message_revision = 1;
    still_frame.width = 1'280;
    still_frame.height = 722;
    bool still_changed = true;
    expect(message_media_apply_frame_dimensions(&store, still_frame, &still_changed) && !still_changed &&
               music_record && music_record->embeds[0].thumbnail.width == 1'280 &&
               music_record->embeds[0].thumbnail.height == 720,
           "message_embed_still_within_one_percent_keeps_declared_frame");
    still_frame.width = 544;
    still_frame.height = 544;
    expect(message_media_apply_frame_dimensions(&store, still_frame, &still_changed) && still_changed &&
               music_record->embeds[0].thumbnail.width == 1'280 && music_record->embeds[0].thumbnail.height == 1'280 &&
               message_rich_embed_media_select(store, music_record->embeds[0], &music_media) &&
               message_rich_embed_media_extent(music_media, 900.0f, &selected_width, &selected_height) &&
               selected_width == 300.0f && selected_height == 300.0f &&
               message_media_apply_frame_dimensions(&store, still_frame, &still_changed) && !still_changed,
           "message_embed_decoded_square_poster_refits_frame_to_its_own_aspect_once");
    still_frame.source_message_revision = 2;
    expect(message_media_apply_frame_dimensions(&store, still_frame, &still_changed) && !still_changed,
           "message_embed_stale_still_never_changes_or_fails_layout");
    message_rich_store_delete(&store, 902);

    MessageRichRecord* mutable_record = record ? &store.records[store.order[0]] : nullptr;
    if (mutable_record) {
        mutable_record->attachments[0].width = 0;
        mutable_record->attachments[0].height = 0;
        mutable_record->attachments[0].state = static_cast<u8>(mutable_record->attachments[0].state & ~3u);
    }
    bool attachment_dimensions_changed = true;
    expect(!message_rich_store_set_attachment_dimensions(&store, 900, 2, 70, 720, 1'280,
                                                         &attachment_dimensions_changed) &&
               !attachment_dimensions_changed &&
               !message_rich_store_set_attachment_dimensions(&store, 900, 1, 71, 720, 1'280,
                                                             &attachment_dimensions_changed) &&
               !attachment_dimensions_changed &&
               message_rich_store_set_attachment_dimensions(&store, 900, 1, 70, 720, 1'280,
                                                            &attachment_dimensions_changed) &&
               attachment_dimensions_changed &&
               message_rich_store_find(store, 900)->attachments[0].width == 720 &&
               message_rich_store_find(store, 900)->attachments[0].height == 1'280 &&
               (message_rich_store_find(store, 900)->attachments[0].state & 3u) == 3u &&
               message_rich_store_set_attachment_dimensions(&store, 900, 1, 70, 720, 1'280,
                                                            &attachment_dimensions_changed) &&
               !attachment_dimensions_changed,
           "message_rich_store_exact_attachment_display_dimensions");

    AccountMediaFrame proxy_frame{};
    proxy_frame.key = media_asset_attachment(70, MediaAttachmentFormat::png);
    proxy_frame.source_message_id = 900;
    proxy_frame.source_message_revision = 1;
    proxy_frame.kind = AccountMediaFrameKind::png_scanlines;
    proxy_frame.width = 72;
    proxy_frame.height = 128;
    expect(message_media_apply_frame_dimensions(&store, proxy_frame, &attachment_dimensions_changed) &&
               !attachment_dimensions_changed && record->attachments[0].width == 720 &&
               record->attachments[0].height == 1'280,
           "message_static_proxy_preserves_known_source_dimensions");

    reactions[0].name = utf8_literal("updated");
    reactions[0].count = 2;
    reactions[0].normal_count = 2;
    reactions[0].burst_count = 0;
    GatewayMessageEvent reaction_update{};
    reaction_update.id = 900;
    reaction_update.fields = gateway_message_field_reactions;
    reaction_update.reactions = ConstSlice<GatewayMessageReaction>{reactions, 1};
    const MessageRichStoreResult updated = message_rich_store_apply(&store, 900, 2, reaction_update);
    record = message_rich_store_find(store, 900);
    expect(updated.ok() && !updated.inserted && record && record->message_revision == 2 &&
               record->reaction_count == 1 && record->reactions[0].count == 2 &&
               record->attachment_count == 1 && record->embed_count == 1 &&
               record->reference.message_id == 800 &&
               utf8_equal(message_rich_store_string(store, record->reactions[0].name),
                          utf8_literal("updated")),
           "message_rich_store_partial_update_preserves_fields");

    char repeated_name[300]{};
    for (u32 index = 0; index < 300; ++index) {
        repeated_name[index] = static_cast<char>('a' + index % 26);
    }
    reactions[0].name = Utf8View{repeated_name, 300};
    for (u32 revision = 3; revision < 14; ++revision) {
        expect(message_rich_store_apply(&store, 900, revision, reaction_update).ok(),
               "message_rich_store_repeated_update");
    }
    record = message_rich_store_find(store, 900);
    expect(record && record->message_revision == 13 && store.compactions != 0 &&
               record->attachment_count == 1 &&
               message_rich_store_string(store, record->reactions[0].name).bytes == 300,
           "message_rich_store_atomic_bank_compaction");

    reaction_update.rich_truncations = gateway_message_rich_truncated_reactions;
    expect(message_rich_store_apply(&store, 900, 14, reaction_update).ok(),
           "message_rich_store_category_truncation_seed");
    GatewayMessageEvent cleared_reactions{};
    cleared_reactions.id = 900;
    cleared_reactions.fields = gateway_message_field_reactions;
    const bool reactions_cleared = message_rich_store_apply(&store, 900, 15, cleared_reactions).ok();
    record = message_rich_store_find(store, 900);
    expect(reactions_cleared && record &&
               (record->truncations & gateway_message_rich_truncated_reactions) == 0,
           "message_rich_store_replacement_clears_category_truncation");

    char oversized_component[3 * 1024]{};
    for (u32 index = 0; index < 3 * 1024; ++index) {
        oversized_component[index] = 'x';
    }
    GatewayMessageComponent oversized[1]{};
    oversized[0].type = 10;
    oversized[0].parent = 0xFFFFu;
    oversized[0].content = Utf8View{oversized_component, 3 * 1024};
    GatewayMessageEvent component_update{};
    component_update.id = 900;
    component_update.fields = gateway_message_field_components;
    component_update.components = ConstSlice<GatewayMessageComponent>{oversized, 1};
    const MessageRichStoreResult degraded = message_rich_store_apply(&store, 900, 16, component_update);
    record = message_rich_store_find(store, 900);
    expect(degraded.ok() && degraded.storage_truncated && record && record->component_count == 1 &&
               record->components[0].content.bytes == 0 &&
               (record->truncations & gateway_message_rich_truncated_text) != 0 &&
               record->attachment_count == 1,
           "message_rich_store_explicit_text_degradation");

    const MessageRichReactionMutation add_self{44,
                                               utf8_literal("party"),
                                               MessageReactionType::normal,
                                               MessageRichReactionMutationKind::add,
                                               true,
                                               true};
    const MessageRichStoreResult self_added = message_rich_store_mutate_reaction(&store, 900, 16, add_self);
    const u32 updates_after_add = store.updated;
    const MessageRichStoreResult duplicate_self =
        message_rich_store_mutate_reaction(&store, 900, 16, add_self);
    MessageRichReactionMutation remote_add = add_self;
    remote_add.current_user = false;
    const MessageRichStoreResult remote_added =
        message_rich_store_mutate_reaction(&store, 900, 16, remote_add);
    MessageRichReactionMutation remove_self = add_self;
    remove_self.kind = MessageRichReactionMutationKind::remove;
    const MessageRichStoreResult self_removed =
        message_rich_store_mutate_reaction(&store, 900, 16, remove_self);
    record = message_rich_store_find(store, 900);
    const bool delta_state = self_added.ok() && duplicate_self.ok() &&
                             store.updated == updates_after_add + 2 && remote_added.ok() &&
                             self_removed.ok() && record && record->reaction_count == 1 &&
                             record->reactions[0].count == 1 && record->reactions[0].normal_count == 1 &&
                             (record->reactions[0].flags & 1u) == 0 && record->attachment_count == 1;
    MessageRichReactionMutation remove_emoji = add_self;
    remove_emoji.kind = MessageRichReactionMutationKind::remove_emoji;
    const MessageRichStoreResult emoji_removed =
        message_rich_store_mutate_reaction(&store, 900, 16, remove_emoji);
    record = message_rich_store_find(store, 900);
    expect(delta_state && emoji_removed.ok() && record && record->reaction_count == 0 &&
               record->attachment_count == 1 && record->component_count == 1,
           "message_rich_store_reaction_deltas_are_idempotent_and_preserve_rich_fields");
    expect(message_rich_store_mutate_reaction(&store, 900, 16, add_self).ok(),
           "message_rich_store_custom_rename_seed");
    auto renamed = add_self;
    renamed.name = utf8_literal("renamed");
    const u32 before_rename = store.updated;
    const auto renamed_self = message_rich_store_mutate_reaction(&store, 900, 16, renamed);
    record = message_rich_store_find(store, 900);
    expect(renamed_self.ok() && store.updated == before_rename && record && record->reaction_count == 1 &&
               record->reactions[0].count == 1 && (record->reactions[0].flags & 1u),
           "message_rich_store_custom_rename_keeps_self_echo_idempotent");
    renamed.current_user = false;
    const auto renamed_foreign = message_rich_store_mutate_reaction(&store, 900, 16, renamed);
    record = message_rich_store_find(store, 900);
    expect(renamed_foreign.ok() && record && record->reaction_count == 1 && record->reactions[0].count == 2,
           "message_rich_store_custom_rename_keeps_foreign_count_on_same_identity");
    auto unnamed = add_self; unnamed.name = {};
    expect(message_rich_store_mutate_reaction(&store, 900, 16, unnamed).ok() &&
        message_rich_store_find(store, 900)->reaction_count == 1 &&
        message_rich_store_find(store, 900)->reactions[0].count == 2 &&
        utf8_equal(message_rich_store_string(store, message_rich_store_find(store, 900)->reactions[0].name),
                   utf8_literal("party")),
        "message_rich_store_unnamed_custom_self_echo_preserves_existing_name_and_count");
    renamed.kind = MessageRichReactionMutationKind::remove_emoji;
    expect(message_rich_store_mutate_reaction(&store, 900, 16, renamed).ok() &&
               message_rich_store_find(store, 900)->reaction_count == 0,
           "message_rich_store_custom_rename_removal_uses_snowflake_identity");
    unnamed.emoji_id = 45;
    expect(message_rich_store_mutate_reaction(&store, 900, 16, unnamed).ok() &&
        message_rich_store_find(store, 900)->reactions[0].emoji_id == 45 &&
        message_rich_store_find(store, 900)->reactions[0].name.bytes == 0,
        "message_rich_store_new_unnamed_custom_preserves_unknown_wire_metadata");
    unnamed.kind = MessageRichReactionMutationKind::remove;
    expect(message_rich_store_mutate_reaction(&store, 900, 16, unnamed).ok() &&
        message_rich_store_find(store, 900)->reaction_count == 0,
        "message_rich_store_unnamed_custom_remove_remains_idempotent_by_snowflake");
    unnamed.emoji_id = 0;
    expect(!message_rich_store_mutate_reaction(&store, 900, 16, unnamed).ok(),
        "message_rich_store_unknown_unicode_identity_remains_rejected");

    GatewayMessageEmbedField inline_fields[3]{};
    inline_fields[0].name = utf8_literal("First");
    inline_fields[0].value = utf8_literal("A");
    inline_fields[0].inline_field = true;
    inline_fields[1].name = utf8_literal("Second");
    inline_fields[1].value = utf8_literal("B");
    inline_fields[1].inline_field = true;
    inline_fields[2].name = utf8_literal("Third");
    inline_fields[2].value = utf8_literal("C");
    inline_fields[2].inline_field = true;
    embeds[0].field_count = 3;
    GatewayMessageEvent field_update{};
    field_update.id = 900;
    field_update.fields = gateway_message_field_embeds;
    field_update.embeds = ConstSlice<GatewayMessageEmbed>{embeds, 1};
    field_update.embed_fields = ConstSlice<GatewayMessageEmbedField>{inline_fields, 3};
    const bool inline_fields_applied = message_rich_store_apply(&store, 900, 17, field_update).ok();
    record = message_rich_store_find(store, 900);
    const MessageRichItemLayout inline_field_layout =
        record ? message_rich_estimate_item_layout(&store, record, 520.0f) : MessageRichItemLayout{};
    for (u32 index = 0; index < 3; ++index) {
        inline_fields[index].inline_field = false;
    }
    const bool stacked_fields_applied = message_rich_store_apply(&store, 900, 18, field_update).ok();
    record = message_rich_store_find(store, 900);
    const MessageRichItemLayout stacked_field_layout =
        record ? message_rich_estimate_item_layout(&store, record, 520.0f) : MessageRichItemLayout{};
    expect(inline_fields_applied && stacked_fields_applied && record && record->embed_field_count == 3 &&
               stacked_field_layout.heights[kMessageRichEmbedItemOffset] ==
                   inline_field_layout.heights[kMessageRichEmbedItemOffset] + 2.0f * (kMessageEmbedBlockGap + 40.0f),
           "message_rich_inline_embed_fields_share_one_estimated_row");

    const char invalid_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    reactions[0].name = Utf8View{invalid_bytes, 2};
    expect(message_rich_store_apply(&store, 900, 19, reaction_update).error ==
               MessageRichStoreError::invalid_utf8,
           "message_rich_store_reject_invalid_utf8");
    expect(message_rich_store_delete(&store, 901) &&
               message_rich_store_delete(&store, 900) && store.count == 0 &&
               !message_rich_store_find(store, 900) && message_rich_store_delete(&store, 900) &&
               message_rich_store_reset(&store) && store.count == 0 && store.text_used == 0,
           "message_rich_store_delete_and_reset");
}

void test_message_draft_store() noexcept {
    MessageDraftRecord records[3]{};
    u8 text[24]{};
    MessageDraftStore store{};
    expect(message_draft_store_initialize(&store, Slice<MessageDraftRecord>{records, 3},
                                          Slice<u8>{text, sizeof(text)}),
           "message_draft_store_initialize");
    {
        // The sparse form never writes the (caller-zeroed) text bank, so its
        // pages stay uncommitted until a draft is saved; a sentinel proves it.
        MessageDraftRecord sparse_records[2]{};
        u8 untouched[16];
        for (u8& byte : untouched)
            byte = 0x5A;
        MessageDraftStore sparse{};
        bool kept = message_draft_store_initialize_sparse(&sparse, Slice<MessageDraftRecord>{sparse_records, 2},
                                                          Slice<u8>{untouched, sizeof(untouched)});
        for (const u8 byte : untouched)
            kept = kept && byte == 0x5A;
        expect(kept && sparse.capacity == 2 && sparse.text_capacity == 16 && sparse.count == 0,
               "message_draft_store_initialize_sparse_leaves_text_bank_untouched");
    }

    const MessageDraftStoreResult first =
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("alpha"), 1, 5});
    const MessageDraftStoreResult second =
        message_draft_store_save(&store, MessageDraftInput{8, utf8_literal("beta"), 4, 4});
    MessageDraftSnapshot snapshot{};
    expect(first.ok() && first.changed && second.ok() && second.changed && store.count == 2 &&
               message_draft_store_snapshot(store, 7, &snapshot) && snapshot.channel_id == 7 &&
               snapshot.anchor == 1 && snapshot.caret == 5 &&
               utf8_equal(snapshot.text, utf8_literal("alpha")),
           "message_draft_store_save_and_snapshot");

    const MessageDraftStoreResult shrunk =
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("a"), 0, 1});
    const MessageDraftStoreResult grown =
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("0123456789abcdef"), 16, 16});
    MessageDraftSnapshot first_after_compaction{};
    MessageDraftSnapshot second_after_compaction{};
    expect(shrunk.ok() && grown.ok() && store.compactions == 1 && store.text_wasted == 0 &&
               store.text_used == 20 && message_draft_store_snapshot(store, 7, &first_after_compaction) &&
               message_draft_store_snapshot(store, 8, &second_after_compaction) &&
               utf8_equal(first_after_compaction.text, utf8_literal("0123456789abcdef")) &&
               utf8_equal(second_after_compaction.text, utf8_literal("beta")),
           "message_draft_store_update_and_in_place_compaction");

    const Utf8View rocket = utf8_literal("🚀");
    expect(message_draft_store_save(&store, MessageDraftInput{9, rocket, 1, rocket.bytes}).error ==
                   MessageDraftStoreError::invalid_utf8 &&
               message_draft_store_save(&store, MessageDraftInput{9, utf8_literal("x"), 1, 1}).ok() &&
               message_draft_store_save(&store, MessageDraftInput{10, utf8_literal("y"), 1, 1}).error ==
                   MessageDraftStoreError::record_capacity,
           "message_draft_store_boundary_and_record_capacity");

    expect(message_draft_store_delete(&store, 7) && message_draft_store_delete(&store, 7) &&
               store.count == 2 &&
               message_draft_store_find(store, 7).error == MessageDraftStoreError::not_found &&
               message_draft_store_reset(&store) && store.count == 0 && store.text_used == 0 &&
               store.text_wasted == 0 && text[0] == 0,
           "message_draft_store_delete_and_reset");

    const MessageDraftStoreResult reply_text = message_draft_store_save(
        &store, MessageDraftInput{7, utf8_literal("alpha"), 1, 5, 701, false});
    const MessageDraftStoreResult reply_only = message_draft_store_save(
        &store, MessageDraftInput{8, Utf8View{}, 0, 0, 801});
    const MessageDraftStoreResult other_reply = message_draft_store_save(
        &store, MessageDraftInput{9, utf8_literal("beta"), 4, 4, 901, false});
    expect(reply_text.ok() && reply_only.ok() && other_reply.ok() && store.count == 3 &&
        message_draft_store_snapshot(store, 8, &snapshot) && snapshot.text.bytes == 0 &&
        snapshot.reply_message_id == 801 && snapshot.mention_reply_author,
        "message_draft_store_reply_only_uses_record_without_text_and_defaults_to_ping");
    const u32 used_before_metadata = store.text_used;
    const u32 revision_before_metadata = store.revision;
    const MessageDraftStoreResult target_changed = message_draft_store_save(
        &store, MessageDraftInput{7, utf8_literal("alpha"), 1, 5, 702, false});
    const MessageDraftStoreResult policy_changed = message_draft_store_save(
        &store, MessageDraftInput{7, utf8_literal("alpha"), 1, 5, 702, true});
    const MessageDraftStoreResult unchanged_reply = message_draft_store_save(
        &store, MessageDraftInput{7, utf8_literal("alpha"), 1, 5, 702, true});
    expect(target_changed.ok() && target_changed.changed && policy_changed.ok() && policy_changed.changed &&
        unchanged_reply.ok() && !unchanged_reply.changed && store.revision == revision_before_metadata + 2 &&
        store.text_used == used_before_metadata && store.text_wasted == 0 && store.compactions == 0,
        "message_draft_store_reply_metadata_changes_are_dirty_without_text_rewrite");
    const MessageDraftStoreResult rejected_reply = message_draft_store_save(
        &store, MessageDraftInput{7, utf8_literal("0123456789abcdefghijklmn"), 0, 24, 703, false});
    expect(rejected_reply.error == MessageDraftStoreError::text_capacity &&
        message_draft_store_snapshot(store, 7, &snapshot) && snapshot.reply_message_id == 702 &&
        snapshot.mention_reply_author && utf8_equal(snapshot.text, utf8_literal("alpha")),
        "message_draft_store_rejected_text_replacement_preserves_reply_policy");
    const MessageDraftStoreResult wrong_target = message_draft_store_clear_reply(&store, 7, 701);
    const MessageDraftStoreResult wrong_channel = message_draft_store_clear_reply(&store, 77, 702);
    expect(wrong_target.ok() && !wrong_target.changed && wrong_channel.ok() && !wrong_channel.changed &&
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("a"), 0, 1, 702}).ok() &&
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("0123456789abcdef"), 2, 16, 702}).ok() &&
        store.compactions == 1 && message_draft_store_snapshot(store, 8, &snapshot) &&
        snapshot.text.bytes == 0 && snapshot.reply_message_id == 801 &&
        message_draft_store_snapshot(store, 9, &snapshot) && snapshot.reply_message_id == 901 &&
        !snapshot.mention_reply_author && utf8_equal(snapshot.text, utf8_literal("beta")),
        "message_draft_store_exact_reply_scope_and_reply_only_survive_compaction");
    const MessageDraftStoreResult cleared_text = message_draft_store_clear_reply(&store, 7, 702);
    const MessageDraftStoreResult cleared_only = message_draft_store_clear_reply(&store, 8, 801);
    expect(cleared_text.ok() && cleared_text.changed && cleared_only.ok() &&
        cleared_only.changed && message_draft_store_find(store, 8).error == MessageDraftStoreError::not_found &&
        store.count == 2 &&
        message_draft_store_snapshot(store, 7, &snapshot) && snapshot.reply_message_id == 0 &&
        snapshot.mention_reply_author && snapshot.anchor == 2 && snapshot.caret == 16 &&
        utf8_equal(snapshot.text, utf8_literal("0123456789abcdef")),
        "message_draft_store_clear_reply_preserves_text_selection_and_erases_reply_only");
    expect(message_draft_store_save(&store, MessageDraftInput{9, Utf8View{}, 0, 0, 901}).ok() &&
        message_draft_store_save(&store, MessageDraftInput{9, Utf8View{}, 0, 0}).changed &&
        message_draft_store_find(store, 9).error == MessageDraftStoreError::not_found &&
        message_draft_store_save(&store, MessageDraftInput{7, utf8_literal("ordinary"), 0, 8, 0, false}).ok() &&
        message_draft_store_snapshot(store, 7, &snapshot) && snapshot.mention_reply_author &&
        message_draft_store_clear_reply(&store, 7, 0).error == MessageDraftStoreError::invalid_input &&
        message_draft_store_reset(&store) && store.count == 0 && store.text_used == 0 &&
        records[0].reply_message_id == 0,
        "message_draft_store_reply_removal_normalizes_policy_and_reset_clears_targets");
    // Reset scrubs only the live prefix; the tail invariant keeps the rest zero.
    bool scrubbed = true;
    for (const u8 byte : text)
        scrubbed = scrubbed && byte == 0;
    expect(scrubbed, "message_draft_store_reset_leaves_no_draft_text_behind");
}

void test_guild_member_store() noexcept {
    GuildMemberRecord records[4]{};
    u32 order[4]{};
    GuildMemberStore store{};
    ChatStateStore state{};
    const bool initialized = guild_member_store_initialize(
        &store, Slice<GuildMemberRecord>{records, 4}, Slice<u32>{order, 4});
    const GuildMemberStoreResult first =
        initialized ? guild_member_store_upsert(&store, state, 7, 11, utf8_literal("first"),
                                                        ConstSlice<u64>{}, false,
                                                        GuildMemberStatus::online, 2)
                    : GuildMemberStoreResult{};
    const GuildMemberStoreResult second =
        first.ok()
            ? guild_member_store_upsert(&store, state, 7, 12, utf8_literal("second"),
                                                ConstSlice<u64>{}, false, GuildMemberStatus::idle, 3)
            : GuildMemberStoreResult{};
    const u32 revision_before = store.revision;
    const bool invalidated =
        second.ok() && guild_member_store_invalidate_list_range(&store, 7, 2, 2);
    const GuildMemberRecord* retained_first = guild_member_store_find(store, 7, 11);
    const GuildMemberRecord* retained_second = guild_member_store_find(store, 7, 12);
    expect(invalidated && store.count == 2 && retained_first && retained_first->list_index == ~0u &&
               retained_second && retained_second->list_index == 3 &&
               store.revision == revision_before + 1 &&
               utf8_equal(guild_member_store_nickname(*retained_first), utf8_literal("first")),
           "guild_member_lazy_invalidate_preserves_cached_identity");
    const u32 stable_revision = store.revision;
    expect(guild_member_store_invalidate_list_range(&store, 7, 20, 30) &&
               store.revision == stable_revision,
           "guild_member_empty_lazy_invalidate_is_stable");
}

void test_member_list_subscription_scope() noexcept {
    GuildMemberRecord member_records[2]{};
    u32 member_order[2]{0, 1};
    GuildMemberStore members{};
    members.records = member_records;
    members.order = member_order;
    members.capacity = 2;
    members.slot_extent = 2;
    members.count = 2;
    members.revision = 1;
    member_records[0].guild_id = 7;
    member_records[0].list_id = 70;
    member_records[0].user_id = 11;
    member_records[0].list_index = 0;
    member_records[0].status = GuildMemberStatus::online;
    member_records[0].occupied = true;
    member_records[1].guild_id = 7;
    member_records[1].list_id = 71;
    member_records[1].user_id = 12;
    member_records[1].list_index = ~0u;
    member_records[1].status = GuildMemberStatus::online;
    member_records[1].occupied = true;

    UserRecord user_records[2]{};
    u32 user_order[2]{0, 1};
    u8 user_names[1]{};
    UserStore users{};
    users.records = user_records;
    users.order = user_order;
    users.names = user_names;
    users.capacity = 2;
    users.count = 2;
    users.name_bank_bytes = sizeof(user_names);
    users.revision = 1;
    user_records[0].id = 11;
    user_records[0].occupied = true;
    user_records[1].id = 12;
    user_records[1].occupied = true;

    GuildRecord guilds[1]{};
    ChatStateStore state{};
    state.guilds = guilds;
    MemberListRow rows[4]{};
    u32 scratch[2]{};
    MemberListView projection{};
    const bool initialized = member_list_view_initialize(
        &projection, Slice<MemberListRow>{rows, 4}, Slice<u32>{scratch, 2});
    const MemberListViewResult rebuilt =
        initialized ? member_list_view_rebuild(&projection, members, users, state, 7, 70)
                    : MemberListViewResult{};
    expect(initialized && rebuilt.ok() && rebuilt.members == 1 && rebuilt.rows == 2 &&
               rows[1].kind == MemberListRowKind::member && rows[1].user_id == 11,
           "member_list_projects_only_current_subscription_indices");
}

// Free-slot hints keep READY insertion O(1) while removals still recycle the
// lowest free slot exactly as the former slot-zero scan did.
void test_chat_state_free_slot_hints() noexcept {
    ChatStateStore state{};
    GuildRecord guilds[4]{};
    ChannelRecord channels[8]{};
    u16 guild_order[4]{}, channel_order[8]{};
    u8 names[1024]{};
    bool valid = chat_state_store_initialize(&state, {guilds, 4}, {guild_order, 4}, {channels, 8},
                                                {channel_order, 8}, {names, 1024});
    for (u32 i = 0; valid && i < 4; ++i)
        valid = chat_state_store_upsert_guild(&state, {100 + i, utf8_literal("Guild"),
                                                          GuildMutationKind::create, 1}).ok();
    for (u32 i = 0; valid && i < 8; ++i)
        valid = chat_state_store_upsert_channel(&state, {10 + i, 100, 0, utf8_literal("chan"), i, 0, 0}).ok();
    const bool filled = valid && state.guild_free_hint == 4 && state.channel_free_hint == 8 &&
                        !chat_state_store_upsert_channel(&state, {99, 100, 0, utf8_literal("x"), 0, 0, 0}).ok();
    valid = filled && chat_state_store_remove_channel(&state, 15) && chat_state_store_remove_channel(&state, 12) &&
            state.channel_free_hint == 2 && chat_state_store_remove_guild(&state, 102, false) &&
            state.guild_free_hint == 2 &&
            chat_state_store_upsert_channel(&state, {30, 100, 0, utf8_literal("new"), 0, 0, 0}).ok() &&
            chat_state_store_upsert_channel(&state, {31, 101, 0, utf8_literal("new"), 0, 0, 0}).ok() &&
            chat_state_store_upsert_guild(&state, {200, utf8_literal("Late"), GuildMutationKind::create, 1}).ok();
    const ChannelRecord* first = valid ? chat_state_store_find_channel(state, 30) : nullptr;
    const ChannelRecord* second = valid ? chat_state_store_find_channel(state, 31) : nullptr;
    const GuildRecord* guild = valid ? chat_state_store_find_guild(state, 200) : nullptr;
    expect(valid && first == &channels[2] && second == &channels[5] && guild == &guilds[2] &&
               state.channel_free_hint == 3 && state.channel_count == 8 && state.guild_count == 4,
           "chat_state_free_slot_hints_recycle_lowest_slot");
}

} // namespace maicord

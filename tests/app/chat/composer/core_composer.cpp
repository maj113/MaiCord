#include "../../../support/test_main_shared.hpp"

#include "../../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "../../../support/layout_validation.hpp"
#include "maicord/app/chat/composer/message_composer_controller.hpp"
#include "../../../support/send_tracker_probe.hpp"
#include "maicord/protocol/messages/edit.hpp"
#include "maicord/protocol/messages/typing.hpp"
#include "maicord/protocol/messages/delete.hpp"
#include "maicord/ui/chat/composer/autocomplete.hpp"
#include "maicord/ui/chat/messages/typing_presentation.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/chat/messages/reaction_picker.hpp"
#include "maicord/ui/shell/text_presentation.hpp"

namespace maicord {
void test_message_composer() noexcept {
    char storage[128]{};
    MessageComposer composer{};
    expect(message_composer_initialize(&composer, Slice<char>{storage, 128}) && composer.capacity == 128 &&
               composer.bytes == 0,
           "message_composer_initialize");
    message_composer_reset(&composer, 42, false);
    expect(message_composer_insert_utf8(&composer, utf8_literal("hello")) ==
                   MessageComposerResult::changed &&
               message_composer_insert_line_break(&composer) == MessageComposerResult::changed &&
               message_composer_insert_utf16(&composer, 0xD83Du) == MessageComposerResult::unchanged &&
               message_composer_insert_utf16(&composer, 0xDE80u) == MessageComposerResult::changed &&
               utf8_equal(message_composer_text(composer), Utf8View{"hello\n\xF0\x9F\x9A\x80", 10}),
           "message_composer_multiline_unicode");
    const u32 move_revision = composer.revision;
    expect(message_composer_move(&composer, MessageComposerMove::previous, false) ==
                   MessageComposerResult::changed &&
               composer.revision == move_revision + 1 &&
               message_composer_backspace(&composer) == MessageComposerResult::changed &&
               composer.revision == move_revision + 2 &&
               utf8_equal(message_composer_text(composer), utf8_literal("hello\xF0\x9F\x9A\x80")),
           "message_composer_scalar_navigation_and_delete");
    const u32 selection_revision = composer.revision;
    expect(message_composer_set_selection(&composer, 5, 9) == MessageComposerResult::changed &&
               composer.revision == selection_revision + 1 &&
               message_composer_set_selection(&composer, 6, 9) == MessageComposerResult::invalid_input &&
               composer.anchor == 5 && composer.caret == 9 && composer.revision == selection_revision + 1 &&
               message_composer_select_all(&composer) == MessageComposerResult::changed &&
               composer.revision == selection_revision + 2 &&
               message_composer_insert_utf8(&composer, utf8_literal("one\ntwo\nthree")) ==
                   MessageComposerResult::changed &&
               message_composer_preferred_height(composer, 300.0f) > kMessageComposerMinimumHeight &&
               message_composer_can_send(composer, true) && !message_composer_can_send(composer, false),
           "message_composer_selection_height_and_send_policy");
    const u32 word_revision = composer.revision;
    expect(message_composer_select_word(&composer, 5) == MessageComposerResult::changed &&
               composer.revision == word_revision + 1 && composer.anchor == 4 && composer.caret == 7 &&
               message_composer_select_word(&composer, 999) == MessageComposerResult::invalid_input &&
               composer.anchor == 4 && composer.caret == 7,
           "message_composer_double_click_word_selection");
    const u32 replace_revision = composer.revision;
    expect(message_composer_replace_range(&composer, 4, 7, utf8_literal("<@42> ")) ==
                   MessageComposerResult::changed &&
               composer.revision == replace_revision + 1 && composer.anchor == 10 && composer.caret == 10 &&
               utf8_equal(message_composer_text(composer), utf8_literal("one\n<@42> \nthree")) &&
               message_composer_replace_range(&composer, 999, 1'000, utf8_literal("x")) ==
                   MessageComposerResult::invalid_input &&
               composer.anchor == 10 && composer.caret == 10,
           "message_composer_atomic_token_replacement");

    MessageComposerScene scene{};
    const UiRect viewport{300.0f, 700.0f, 1'000.0f, 780.0f};
    const MessageComposerSceneResult materialized =
        message_composer_scene_initialize_owned(&scene)
            ? message_composer_scene_update(&scene, &composer, viewport, true)
            : MessageComposerSceneResult{};
    const UiLayoutNode* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
    const UiLayoutNode* send = message_composer_scene_layout_node(scene, MessageComposerControl::send);
    UiLayoutDiagnostic diagnostics[16]{};
    const UiLayoutValidationResult validation =
        ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16});
    expect(materialized.ok() && scene.active && scene.widgets.count == kMessageComposerWidgetCapacity && edit && send &&
               edit->rectangle.right <= send->rectangle.left &&
               message_composer_scene_hit_test(scene, (edit->rectangle.left + edit->rectangle.right) * 0.5f,
                                               (edit->rectangle.top + edit->rectangle.bottom) * 0.5f) ==
                   MessageComposerControl::edit &&
               message_composer_scene_focus_edit(&scene) && validation.clean(),
           "message_composer_retained_scene_layout_hit_and_validate");

    const Utf8View reply_label = utf8_literal("Replying to Wren042 - click to cancel");
    const MessageComposerSceneResult reply_materialized =
        message_composer_scene_update(&scene, &composer, viewport, true, 77, reply_label);
    const UiLayoutNode* reply =
        message_composer_scene_layout_node(scene, MessageComposerControl::cancel_reply);
    expect(reply_materialized.ok() && reply_materialized.changed && reply &&
               ui_rect_height(reply->rectangle) == 28.0f &&
               utf8_equal(message_composer_scene_reply_label(scene), reply_label) &&
               message_composer_scene_hit_test(scene,
                                               (reply->rectangle.left + reply->rectangle.right) * 0.5f,
                                               (reply->rectangle.top + reply->rectangle.bottom) * 0.5f) ==
                   MessageComposerControl::cancel_reply,
           "message_composer_reply_banner_layout_hit_and_text");
    const Utf8View same_size_reply = utf8_literal("Replying to QuillFern - tap to cancel");
    expect(
        same_size_reply.bytes == reply_label.bytes &&
            message_composer_scene_update(&scene, &composer, viewport, true, 77, same_size_reply).changed &&
            utf8_equal(message_composer_scene_reply_label(scene), same_size_reply),
        "message_composer_reply_banner_same_size_text_change");
    const Utf8View typing_label = utf8_literal("QuillFern is typing…");
    const MessageComposerSceneResult typing_materialized = message_composer_scene_update(
        &scene, &composer, viewport, true, 77, same_size_reply, nullptr, typing_label);
    const UiLayoutNode* typing =
        message_composer_scene_layout_node(scene, MessageComposerControl::typing_indicator);
    expect(typing_materialized.ok() && typing_materialized.changed && typing &&
               ui_rect_height(typing->rectangle) == 20.0f &&
               utf8_equal(message_composer_scene_typing_label(scene), typing_label),
           "message_composer_typing_row_is_fixed_and_retained");

    UserRecord projection_user_records[2]{};
    u32 projection_user_order[2]{};
    u8 projection_user_names[128]{};
    UserStore projection_users{};
    GuildRecord projection_guilds[1]{};
    ChannelRecord projection_channels[2]{};
    RoleRecord projection_roles[2]{};
    PermissionOverwriteRecord projection_overwrites[2]{};
    u16 projection_guild_order[1]{};
    u16 projection_channel_order[2]{};
    u8 projection_names[512]{};
    ChatStateStore projection_state{};
    const bool projection_state_ready =
        user_store_initialize(
            &projection_users, Slice<UserRecord>{projection_user_records, 2},
            Slice<u32>{projection_user_order, 2}, Slice<u8>{projection_user_names, 128}) &&
        user_store_upsert(&projection_users, 42, utf8_literal("Wren042")).ok() &&
        chat_state_store_initialize(
            &projection_state, Slice<GuildRecord>{projection_guilds, 1},
            Slice<u16>{projection_guild_order, 1}, Slice<ChannelRecord>{projection_channels, 2},
            Slice<u16>{projection_channel_order, 2}, Slice<u8>{projection_names, 512}) &&
        chat_state_store_bind_permissions(
            &projection_state, Slice<RoleRecord>{projection_roles, 2},
            Slice<PermissionOverwriteRecord>{projection_overwrites, 2}) &&
        chat_state_store_upsert_guild(
            &projection_state,
            GuildInput{77, utf8_literal("Guild"), GuildMutationKind::create, 1})
            .ok() &&
        chat_state_store_upsert_channel(&projection_state,
                                           ChannelInput{99, 77, 0, utf8_literal("general"), 0, 0, 0})
            .ok() &&
        chat_state_store_upsert_role(&projection_state, RoleInput{55, 77, 0, utf8_literal("Mods"),
                                                                            1, 0x00FF40u, true, false});
    const bool projection_model_ready =
        message_composer_clear(&composer) == MessageComposerResult::changed &&
        message_composer_insert_utf8(&composer, utf8_literal("hi <@42> <@&55> <#99>")) ==
            MessageComposerResult::changed;
    const MessageComposerProjectionContext projection_context{&projection_users, &projection_state, 77};
    const MessageComposerSceneResult projection_result =
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context);
    expect(projection_state_ready && projection_model_ready && projection_result.ok() &&
               scene.projection_span_count == 3 &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Wren042 @Mods #general")) &&
               message_composer_scene_display_offset_from_model(scene, 3) == 3 &&
               message_composer_scene_display_offset_from_model(scene, 8) == 11 &&
               message_composer_scene_model_offset_from_display(scene, 3) == 3 &&
               message_composer_scene_model_offset_from_display(scene, 11) == 8 &&
               scene.projection_spans[1].kind == MessageComposerProjectionKind::role &&
               scene.projection_spans[1].color == 0x00FF40u,
           "message_composer_id_tokens_project_as_semantic_labels_with_mapping");

    const u64 initial_projection_signature = scene.display_signature;
    const bool projection_user_renamed =
        user_store_upsert(&projection_users, 42, utf8_literal("Robin11")).ok();
    const MessageComposerSceneResult renamed_projection =
        projection_user_renamed
            ? message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context)
            : MessageComposerSceneResult{};
    const u64 renamed_projection_signature = scene.display_signature;
    const bool projection_role_recolored = chat_state_store_upsert_role(
        &projection_state, RoleInput{55, 77, 0, utf8_literal("Mods"), 1, 0xF05ABAu, true, false});
    const MessageComposerSceneResult recolored_projection =
        projection_role_recolored
            ? message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context)
            : MessageComposerSceneResult{};
    expect(projection_user_renamed && renamed_projection.ok() && renamed_projection.changed &&
               renamed_projection_signature != initial_projection_signature &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Robin11 @Mods #general")),
           "message_composer_same_length_projected_name_change_invalidates");
    expect(projection_role_recolored && recolored_projection.ok() && recolored_projection.changed &&
               scene.display_signature != renamed_projection_signature &&
               scene.projection_spans[1].color == 0xF05ABAu,
           "message_composer_projected_role_color_change_invalidates");

    const auto stable_role_projection = [&]() noexcept {
        const u32 projected = scene.projection_passes, laid_out = scene.layout_passes;
        const u32 revision = composer.revision, anchor = composer.anchor, caret = composer.caret;
        bool stable = true;
        for (u32 frame = 0; frame < 64; ++frame) {
            const auto update = message_composer_scene_update(&scene, &composer, viewport, true, 0, {},
                                                              &projection_context);
            stable &= update.ok() && !update.changed && scene.projection_passes == projected &&
                      scene.layout_passes == laid_out && composer.revision == revision &&
                      composer.anchor == anchor && composer.caret == caret;
        }
        return stable;
    };
    expect(stable_role_projection(),
           "message_composer_recolored_role_64_updates_reuse_projection_layout_and_document");
    const u32 before_role_rename = scene.projection_passes;
    const bool role_renamed = chat_state_store_upsert_role(
        &projection_state, RoleInput{55, 77, 0, utf8_literal("Team"), 1, 0xF05ABAu, true, false});
    const auto role_rename_update =
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context);
    expect(role_renamed && role_rename_update.ok() && role_rename_update.changed &&
               scene.projection_passes == before_role_rename + 1 &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Robin11 @Team #general")) &&
               stable_role_projection(),
           "message_composer_same_length_role_rename_invalidates_once_then_reuses_projection");
    bool role_discovered =
        message_composer_insert_utf8(&composer, utf8_literal(" <@&56>")) ==
            MessageComposerResult::changed &&
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context).ok() &&
        scene.projection_span_count == 3 &&
        utf8_equal(message_composer_scene_display_text(scene),
                   utf8_literal("hi @Robin11 @Team #general <@&56>"));
    const u32 before_role_discovery = scene.projection_passes;
    role_discovered &= chat_state_store_upsert_role(
        &projection_state, RoleInput{56, 77, 0, utf8_literal("Fans"), 1, 0x123456u, true, false});
    const auto role_discovery_update =
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context);
    expect(role_discovered && role_discovery_update.ok() && role_discovery_update.changed &&
               scene.projection_passes == before_role_discovery + 1 && scene.projection_span_count == 4 &&
               scene.projection_spans[3].target == 56 && scene.projection_spans[3].color == 0x123456u &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Robin11 @Team #general @Fans")) &&
               stable_role_projection(),
           "message_composer_newly_resolved_role_invalidates_once_and_stabilizes_dependency_set");
    const auto role_document = utf8_literal("hi <@42> <@&55> <#99> <@&56>");
    const u32 role_revision = composer.revision, role_anchor = composer.anchor, role_caret = composer.caret;
    const u32 before_role_remove = scene.projection_passes;
    const bool role_removed = chat_state_store_remove_role(&projection_state, 77, 55);
    const auto role_remove_update =
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context);
    expect(role_removed && role_remove_update.ok() && role_remove_update.changed &&
               scene.projection_passes == before_role_remove + 1 && scene.projection_span_count == 3 &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Robin11 <@&55> #general @Fans")) &&
               stable_role_projection(),
           "message_composer_removed_role_restores_literal_token_without_stale_projection");
    const u32 before_role_reinsert = scene.projection_passes;
    const bool role_reinserted = chat_state_store_upsert_role(
        &projection_state, RoleInput{55, 77, 0, utf8_literal("Team"), 1, 0x654321u, true, false});
    const auto role_reinsert_update =
        message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context);
    expect(role_reinserted && role_reinsert_update.ok() && role_reinsert_update.changed &&
               scene.projection_passes == before_role_reinsert + 1 && scene.projection_span_count == 4 &&
               scene.projection_spans[1].target == 55 && scene.projection_spans[1].color == 0x654321u &&
               utf8_equal(message_composer_scene_display_text(scene),
                          utf8_literal("hi @Robin11 @Team #general @Fans")) &&
               utf8_equal(message_composer_text(composer), role_document) &&
               composer.revision == role_revision && composer.anchor == role_anchor &&
               composer.caret == role_caret && stable_role_projection(),
           "message_composer_reinserted_role_refreshes_color_preserves_selection_and_reuses_projection");

    const Utf8View entities[] = {utf8_literal("<@42>"), utf8_literal("<@&55>"), utf8_literal("<#99>"), utf8_literal("<:party:123>"), utf8_literal("<a:party:123>")};
    bool atomic_edits = message_composer_enable_history(&composer, 4096);
    for (const auto entity : entities) {
        atomic_edits = atomic_edits && message_composer_restore(&composer, 42, entity, entity.bytes, entity.bytes) &&
            message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context).ok() &&
            scene.projection_span_count == 1 && message_composer_backspace(&composer) == MessageComposerResult::changed && composer.bytes == 0 &&
            message_composer_undo(&composer) == MessageComposerResult::changed && utf8_equal(message_composer_text(composer), entity) &&
            message_composer_scene_update(&scene, &composer, viewport, true, 0, {}, &projection_context).ok();
        message_composer_set_selection(&composer, 0, 0);
        atomic_edits = atomic_edits && message_composer_move(&composer, MessageComposerMove::next, false) == MessageComposerResult::changed && composer.caret == entity.bytes;
        message_composer_set_selection(&composer, 0, 0);
        atomic_edits = atomic_edits && message_composer_delete_forward(&composer) == MessageComposerResult::changed && composer.bytes == 0;
    }
    expect(atomic_edits, "composer_entities_backspace_delete_arrow_and_undo_are_atomic");
    const auto emoji_source = utf8_literal("x<:party:123>y");
    bool emoji_projection = message_composer_restore(&composer, 42, emoji_source, 12, 12) &&
        message_composer_scene_update(&scene, &composer, viewport, true).ok() && scene.projection_span_count == 1 &&
        scene.projection_spans[0].kind == MessageComposerProjectionKind::emoji && scene.projection_spans[0].target == 123 &&
        utf8_equal(message_composer_scene_display_text(scene), utf8_literal("x\xEF\xBF\xBCy"));
    const u64 emoji_signature = scene.display_signature;
    const auto other_emoji = utf8_literal("x<:party:124>y");
    emoji_projection = emoji_projection && message_composer_restore(&composer, 42, other_emoji, 12, 12) &&
        message_composer_scene_update(&scene, &composer, viewport, true).ok() && scene.display_signature != emoji_signature;
    message_composer_set_selection(&composer, 5, 8);
    emoji_projection = emoji_projection && message_composer_insert_scalar(&composer, 'z') == MessageComposerResult::changed &&
        utf8_equal(message_composer_text(composer), utf8_literal("xzy"));
    const auto literal_entity = utf8_literal("`<:party:123>`");
    emoji_projection = emoji_projection && message_composer_restore(&composer, 42, literal_entity, 13, 13) &&
        message_composer_scene_update(&scene, &composer, viewport, true).ok() && scene.projection_span_count == 0 &&
        utf8_equal(message_composer_scene_display_text(scene), literal_entity);
    expect(emoji_projection, "composer_custom_emoji_projection_identity_partial_replacement_and_literal_code");

    TypingRecord typing_records[8]{};
    TypingStore typing_store{};
    UserRecord typing_user_records[5]{};
    u32 typing_user_order[5]{};
    u8 typing_user_names[128]{};
    UserStore typing_users{};
    char typing_text[kMessageTypingLabelMaximumBytes]{};
    const bool typing_ready =
        typing_store_initialize(&typing_store, Slice<TypingRecord>{typing_records, 8}) &&
        user_store_initialize(&typing_users, Slice<UserRecord>{typing_user_records, 5},
                                      Slice<u32>{typing_user_order, 5},
                                      Slice<u8>{typing_user_names, 128}) &&
        user_store_upsert(&typing_users, 1, utf8_literal("Self")).ok() &&
        user_store_upsert(&typing_users, 2, utf8_literal("Alice")).ok() &&
        user_store_upsert(&typing_users, 3, utf8_literal("Bob")).ok() &&
        user_store_upsert(&typing_users, 4, utf8_literal("Cara")).ok() &&
        user_store_upsert(&typing_users, 5, utf8_literal("Drew")).ok();
    const TypingStoreResult local_typing =
        typing_store_note_local_input(&typing_store, 10, 20, 1, 100);
    const TypingPost early_typing = typing_store_take_local_post(&typing_store, 1'599);
    const TypingPost ready_typing = typing_store_take_local_post(&typing_store, 1'600);
    const TypingStoreResult optimistic_message =
        typing_store_note_message(&typing_store, 10, 1, true);
    const TypingStoreResult alice_typing = typing_store_start(&typing_store, 10, 20, 2, 200);
    MessageTypingPresentationResult typing_text_result = message_typing_presentation_build(
        typing_store, typing_users, 10, 1, Slice<char>{typing_text, kMessageTypingLabelMaximumBytes});
    const bool single_typing_label = typing_text_result.ok() && typing_text_result.users == 1 &&
                                     utf8_equal(Utf8View{typing_text, typing_text_result.bytes},
                                                utf8_literal("Alice is typing…"));
    typing_store_start(&typing_store, 10, 20, 3, 200);
    typing_store_start(&typing_store, 10, 20, 4, 200);
    MessageTypingName typing_names[3]{};
    typing_text_result = message_typing_presentation_build(
        typing_store, typing_users, 10, 1, Slice<char>{typing_text, kMessageTypingLabelMaximumBytes}, typing_names);
    const bool three_typing_label = typing_text_result.ok() && typing_text_result.users == 3 &&
                                    utf8_equal(Utf8View{typing_text, typing_text_result.bytes},
                                               utf8_literal("Alice, Bob, and Cara are typing…"));
    // Each name's span lets the shell colour it: offset, bytes and the typist's id.
    const bool typing_name_spans =
        typing_names[0].user_id == 2 && typing_names[0].offset == 0 && typing_names[0].bytes == 5 &&
        typing_names[1].user_id == 3 && typing_names[1].offset == 7 && typing_names[1].bytes == 3 &&
        typing_names[2].user_id == 4 && typing_names[2].offset == 16 && typing_names[2].bytes == 4 &&
        utf8_equal(Utf8View{typing_text + typing_names[2].offset, typing_names[2].bytes}, utf8_literal("Cara"));
    typing_store_start(&typing_store, 10, 20, 5, 200);
    typing_text_result = message_typing_presentation_build(
        typing_store, typing_users, 10, 1, Slice<char>{typing_text, kMessageTypingLabelMaximumBytes});
    const bool several_typing_label = typing_text_result.ok() && typing_text_result.users == 4 &&
                                      utf8_equal(Utf8View{typing_text, typing_text_result.bytes},
                                                 utf8_literal("Several people are typing…"));
    const TypingStoreResult before_expiry = typing_store_tick(&typing_store, 10'199);
    const TypingStoreResult at_expiry = typing_store_tick(&typing_store, 10'200);
    expect(typing_ready && local_typing.ok() && local_typing.accepted && !early_typing.ready &&
               ready_typing.ready && ready_typing.channel_id == 10 && !ready_typing.suppressed &&
               optimistic_message.ok() && optimistic_message.changed && alice_typing.ok() &&
               single_typing_label && three_typing_label && typing_name_spans && several_typing_label && before_expiry.ok() &&
               !before_expiry.changed && at_expiry.ok() && at_expiry.changed && typing_store.count == 0,
           "typing_cadence_labels_message_stop_and_expiry");

    u8 target[96]{};
    u8 body[256]{};
    u8 reply_body[512]{};
    const MessageCreateWriteResult target_written =
        write_message_create_target(42, Slice<u8>{target, 96});
    u8 delete_target[96]{};
    const MessageDeleteWriteResult delete_target_written =
        write_message_delete_target(42, 77, Slice<u8>{delete_target, sizeof(delete_target)});
    u8 edit_target[96]{};
    u8 edit_body[256]{};
    const MessageEditWriteResult edit_target_written =
        write_message_edit_target(42, 77, Slice<u8>{edit_target, sizeof(edit_target)});
    const MessageEditWriteResult edit_body_written = write_message_edit_body(
        utf8_literal("edited\n\"text\""), Slice<u8>{edit_body, sizeof(edit_body)});
    u8 reaction_target[kMessageReactionMaximumTargetBytes]{};
    const MessageReactionWriteResult reaction_add_written = write_message_reaction_target(
        42, 77, 0, utf8_literal("👍"), MessageReactionType::normal, true,
        Slice<u8>{reaction_target, sizeof(reaction_target)});
    u8 typing_target[kMessageTypingMaximumTargetBytes]{};
    const MessageTypingWriteResult typing_target_written =
        write_message_typing_target(42, Slice<u8>{typing_target, sizeof(typing_target)});
    const MessageCreateWriteResult body_written =
        write_message_create_body(utf8_literal("a\n\"b\\c"), 99, 0, 0, {}, {}, Slice<u8>{body, 256});
    const MessageCreateWriteResult reply_body_written =
        write_message_create_body(utf8_literal("reply"), 99, 42, 77, {}, {}, Slice<u8>{reply_body, sizeof(reply_body)});
    expect(
        target_written.ok() &&
            bytes_equal(ConstSlice<u8>{target, target_written.bytes},
                        byte_literal("/api/v9/channels/42/messages")) &&
            body_written.ok() &&
            bytes_equal(ConstSlice<u8>{body, body_written.bytes},
                        byte_literal(
                            "{\"content\":\"a\\n\\\"b\\\\c\",\"nonce\":\"99\",\"tts\":false,\"flags\":0,\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}")),
        "message_create_target_and_json_escape");
    expect(
        reply_body_written.ok() &&
            bytes_equal(ConstSlice<u8>{reply_body, reply_body_written.bytes},
                        byte_literal("{\"content\":\"reply\",\"nonce\":\"99\",\"message_reference\":{"
                                     "\"type\":0,\"message_id\":\"77\",\"channel_id\":\"42\",\"fail_if_not_"
                                     "exists\":false},\"allowed_mentions\":{\"parse\":[\"users\",\"roles\","
                                     "\"everyone\"],\"replied_user\":true},\"tts\":false,\"flags\":0,\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}")),
        "message_create_reply_reference_and_ping_policy");
    const auto exact_nonce_body = write_message_create_body(utf8_literal("a\n\"b\\c"), 99, 0, 0, {}, {},
        {body, body_written.bytes});
    const auto short_nonce_body = write_message_create_body(utf8_literal("a\n\"b\\c"), 99, 0, 0, {}, {},
        {body, body_written.bytes - 1});
    expect(exact_nonce_body.ok() && exact_nonce_body.bytes == body_written.bytes &&
        short_nonce_body.error == MessageCreateWriteError::output_too_small && !short_nonce_body.bytes,
        "message_create_nonce_enforcement_exact_capacity_and_truncated_tail");
    expect(delete_target_written.ok() &&
               bytes_equal(ConstSlice<u8>{delete_target, delete_target_written.bytes},
                           byte_literal("/api/v9/channels/42/messages/77")) &&
               write_message_delete_target(0, 77, Slice<u8>{delete_target, sizeof(delete_target)})
                       .error == MessageDeleteWriteError::invalid_input &&
               write_message_delete_target(42, 0, Slice<u8>{delete_target, sizeof(delete_target)})
                       .error == MessageDeleteWriteError::invalid_input,
           "message_delete_target_and_validation");
    expect(
        edit_target_written.ok() && edit_body_written.ok() &&
            bytes_equal(ConstSlice<u8>{edit_target, edit_target_written.bytes},
                        byte_literal("/api/v9/channels/42/messages/77")) &&
            bytes_equal(ConstSlice<u8>{edit_body, edit_body_written.bytes},
                        byte_literal("{\"content\":\"edited\\n\\\"text\\\"\"}")) &&
            write_message_edit_target(0, 77, Slice<u8>{edit_target, sizeof(edit_target)}).error ==
                MessageEditWriteError::invalid_input &&
            write_message_edit_body(Utf8View{nullptr, 1}, Slice<u8>{edit_body, sizeof(edit_body)})
                    .error == MessageEditWriteError::invalid_input,
        "message_edit_patch_target_body_and_validation");
    expect(reaction_add_written.ok() &&
               bytes_equal(ConstSlice<u8>{reaction_target, reaction_add_written.bytes},
                           byte_literal("/api/v9/channels/42/messages/77/reactions/%F0%9F%91%8D/"
                                        "@me?location=Message&type=0")) &&
               write_message_reaction_target(42, 77, 123, utf8_literal("wave"),
                                             MessageReactionType::burst, false,
                                             Slice<u8>{reaction_target, sizeof(reaction_target)})
                   .ok() &&
               bytes_equal(
                   ConstSlice<u8>{reaction_target,
                                  write_message_reaction_target(
                                      42, 77, 123, utf8_literal("wave"), MessageReactionType::burst,
                                      false, Slice<u8>{reaction_target, sizeof(reaction_target)})
                                      .bytes},
                   byte_literal("/api/v9/channels/42/messages/77/reactions/wave:123/1/"
                                "@me?location=Message&burst=true")) &&
               write_message_reaction_target(0, 77, 0, utf8_literal("x"),
                                             MessageReactionType::normal, true,
                                             Slice<u8>{reaction_target, sizeof(reaction_target)})
                       .error == MessageReactionWriteError::invalid_input,
           "message_reaction_add_remove_routes_and_validation");
    expect(
        typing_target_written.ok() &&
            bytes_equal(ConstSlice<u8>{typing_target, typing_target_written.bytes},
                        byte_literal("/api/v9/channels/42/typing")) &&
            write_message_typing_target(0, Slice<u8>{typing_target, sizeof(typing_target)}).error ==
                MessageTypingWriteError::invalid_input,
        "message_typing_route_and_validation");
    {
        u32 structurals[32];
        MessageTypingCooldowns cooldowns{7, 7};
        expect(decode_message_typing_cooldowns(
                   utf8_literal("{\"message_send_cooldown_ms\":4200,\"thread_create_cooldown_ms\":900}"), {structurals, 32},
                   &cooldowns) &&
                   cooldowns.message_send_ms == 4200 && cooldowns.thread_create_ms == 900 &&
                   decode_message_typing_cooldowns(utf8_literal("{}"), {structurals, 32}, &cooldowns) &&
                   cooldowns.message_send_ms == 0 && cooldowns.thread_create_ms == 0 &&
                   !decode_message_typing_cooldowns(utf8_literal("{\"message_send_cooldown_ms\":\"x\"}"),
                                                    {structurals, 32}, &cooldowns) &&
                   !decode_message_typing_cooldowns(utf8_literal("[1]"), {structurals, 32}, &cooldowns) &&
                   !decode_message_typing_cooldowns(Utf8View{}, {structurals, 32}, &cooldowns),
               "message_typing_cooldown_decode");
    }
    expect(write_message_create_target(0, Slice<u8>{target, 96}).error ==
                   MessageCreateWriteError::invalid_input &&
               write_message_create_body(Utf8View{}, 99, 0, 0, {}, {}, Slice<u8>{body, 256}).error ==
                   MessageCreateWriteError::invalid_input &&
               write_message_create_body(utf8_literal("x"), 0, 0, 0, {}, {}, Slice<u8>{body, 256}).error ==
                   MessageCreateWriteError::invalid_input,
           "message_create_validation");

    char long_storage[kMessageComposerMaximumBytes]{};
    MessageComposer long_composer{};
    bool long_ready = message_composer_initialize(&long_composer,
                                                  Slice<char>{long_storage, kMessageComposerMaximumBytes});
    message_composer_reset(&long_composer, 42, false);
    for (u32 index = 0; index < kMessageComposerDefaultMaximumCodepoints && long_ready; ++index) {
        long_ready =
            message_composer_insert_scalar(&long_composer, 0x1F680u) == MessageComposerResult::changed;
    }
    const bool default_limit_accepted = long_ready && message_composer_codepoints(long_composer) == 2'000 &&
                                        long_composer.bytes == 8'000 &&
                                        message_composer_can_send(long_composer, true);
    const bool over_default =
        message_composer_insert_scalar(&long_composer, 0x1F680u) == MessageComposerResult::changed &&
        !message_composer_can_send(long_composer, true) &&
        message_composer_can_send(long_composer, true, 4'000);
    const bool long_projection =
        message_composer_set_selection(&long_composer, long_composer.bytes - 8, long_composer.bytes) ==
            MessageComposerResult::changed &&
        message_composer_scene_update(&scene, &long_composer, viewport, true).ok() &&
        scene.display_first == 0 && scene.display_bytes == long_composer.bytes &&
        scene.display_caret == scene.display_bytes && scene.display_anchor + 8 == scene.display_caret &&
        utf8_validate(message_composer_scene_display_text(scene)).valid;
    expect(default_limit_accepted && over_default && long_projection,
           "message_composer_unicode_character_limits");
    message_composer_scene_release(&scene);
    expect(!scene.owned_storage.base && !scene.display && !scene.projection_spans,
           "message_composer_scene_owned_storage_release");

    MessageComposer owned{};
    const bool reserved_only = message_composer_initialize_owned(&owned, 3 * 4096) && owned.owns_storage &&
        owned.owned_storage.base && owned.text && owned.capacity == 3 * 4096 && !owned.owned_storage.committed;
    expect(reserved_only && message_composer_insert_utf8(&owned, utf8_literal("owned")) ==
                   MessageComposerResult::changed &&
               owned.owned_storage.committed == 4096 && owned.owned_storage.commit_count == 1,
           "message_composer_owned_storage_commits_its_first_page_with_the_first_text");
    static char page_text[4096];
    for (char& c : page_text) c = 'a';
    const bool same_page = message_composer_insert_utf8(&owned, {page_text, 4096 - 6}) ==
        MessageComposerResult::changed && owned.bytes == 4095 && owned.owned_storage.commit_count == 1;
    const bool next_page = message_composer_insert_utf8(&owned, utf8_literal("bc")) ==
        MessageComposerResult::changed && owned.bytes == 4097 && owned.owned_storage.committed == 8192 &&
        owned.owned_storage.commit_count == 2 && message_composer_undo(&owned) == MessageComposerResult::changed &&
        owned.bytes < 4097 && owned.owned_storage.commit_count == 2;
    expect(same_page && next_page, "message_composer_owned_storage_commits_a_page_only_when_the_draft_enters_it");
    message_composer_release(&owned);
    expect(!owned.owned_storage.base && !owned.text && owned.capacity == 0,
           "message_composer_owned_storage_release");
}

namespace {
// The leading ':' suggestions are these emoji, by name: a custom emoji's label
// is its name, a Unicode emoji's ends with ":name:".
bool emoji_suggestions_lead(const ComposerAutocomplete& autocomplete, const Utf8View* names, u32 count) noexcept {
    if (autocomplete.kind != ComposerAutocompleteKind::emoji || autocomplete.count < count)
        return false;
    for (u32 i = 0; i < count; ++i) {
        const Utf8View label = autocomplete.suggestions[i].label;
        const Utf8View name = names[i];
        if (autocomplete.suggestions[i].emoji_id ? !utf8_equal(label, name)
                                                 : label.bytes < name.bytes + 2 || label.data[label.bytes - 1] != ':' ||
                                                       label.data[label.bytes - name.bytes - 2] != ':' ||
                                                       !bytes_equal(label.data + label.bytes - name.bytes - 1, name.data,
                                                                    name.bytes))
            return false;
    }
    return true;
}
u32 emoji_insertions(const ComposerAutocomplete& autocomplete, Utf8View glyph) noexcept {
    u32 found = 0;
    for (u32 i = 0; i < autocomplete.count; ++i)
        found += autocomplete.suggestions[i].insertion_bytes == glyph.bytes + 1 &&
                 bytes_equal(autocomplete.suggestions[i].insertion, glyph.data, glyph.bytes);
    return found;
}
// Synced composer frecency (emoji_frecency): `uses` uses an hour apart before `now_ms`.
ReactionPreferenceRemoteUsage message_use(Utf8View key, u32 uses, u64 now_ms) noexcept {
    ReactionPreferenceRemoteUsage usage{};
    usage.emoji = reaction_emoji_from_settings_key(key);
    usage.total_uses = uses;
    usage.recent_count = static_cast<u8>(uses);
    for (u32 i = 0; i < uses; ++i)
        usage.recent_uses[i] = now_ms - (uses - i) * 3'600'000ull;
    return usage;
}
} // namespace

void test_composer_autocomplete() noexcept {
    char text[128]{};
    MessageComposer composer{};
    UserRecord user_records[2]{};
    u32 user_order[2]{};
    u8 user_names[256]{};
    UserStore users{};
    MemberListRow member_rows[1]{};
    MemberListView members{};
    NavigationView navigation{};
    ChatStateStore state{};
    ComposerAutocomplete autocomplete{};
    const bool initialized =
        message_composer_initialize(&composer, Slice<char>{text, 128}) &&
        user_store_initialize(&users, Slice<UserRecord>{user_records, 2},
                                      Slice<u32>{user_order, 2}, Slice<u8>{user_names, 256}) &&
        user_store_upsert(&users, 42, utf8_literal("Wren042")).ok();
    member_rows[0].kind = MemberListRowKind::member;
    member_rows[0].user_id = 42;
    members.rows = Slice<MemberListRow>{member_rows, 1};
    members.count = 1;
    message_composer_reset(&composer, 7, false);

    const bool bare_member =
        initialized &&
        message_composer_insert_utf8(&composer, utf8_literal("@")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) &&
        autocomplete.kind == ComposerAutocompleteKind::member && autocomplete.count == 1 &&
        autocomplete.token_begin == 0 && autocomplete.token_end == 1 &&
        utf8_equal(autocomplete.suggestions[0].label, utf8_literal("Wren042")) &&
        utf8_equal(
            Utf8View{autocomplete.suggestions[0].insertion, autocomplete.suggestions[0].insertion_bytes},
            utf8_literal("<@42> "));
    expect(bare_member, "composer_autocomplete_bare_member_mention");
    expect(autocomplete.storage.base && autocomplete.storage.committed != 0 && autocomplete.suggestions,
           "composer_autocomplete_storage_materializes_on_first_result_kind");
    const auto styled_name = utf8_literal("𝕼𝖚𝖎𝖑𝖑𝕱𝖊𝖗𝖓");
    member_rows[0].status = GuildMemberStatus::online;
    const bool username_match = user_store_upsert(&users, 42, styled_name, utf8_literal("quillfern")).ok() &&
        message_composer_restore(&composer, 7, utf8_literal("@QUI"), 4, 4) &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) && autocomplete.count == 1 &&
        utf8_equal(autocomplete.suggestions[0].label, styled_name) &&
        utf8_equal(autocomplete.suggestions[0].username, utf8_literal("quillfern")) &&
        autocomplete.suggestions[0].user_id == 42 && autocomplete.suggestions[0].status == GuildMemberStatus::online;
    expect(username_match, "autocomplete_username_matches_styled_display_name_and_preserves_both_columns");
    expect(user_store_upsert(&users, 42, utf8_literal("Renamed")).ok() &&
        utf8_equal(user_store_username(users, *user_store_find(users, 42)), utf8_literal("quillfern")) &&
        utf8_equal(autocomplete.suggestions[0].label, styled_name),
        "partial_display_updates_preserve_username_and_do_not_invalidate_owned_popup_labels");

    const bool one_character_hidden =
        message_composer_clear(&composer) == MessageComposerResult::changed &&
        message_composer_insert_utf8(&composer, utf8_literal(":e")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) &&
        autocomplete.kind == ComposerAutocompleteKind::none && autocomplete.count == 0;
    const bool two_character_visible =
        message_composer_insert_utf8(&composer, utf8_literal("m")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) &&
        autocomplete.kind == ComposerAutocompleteKind::emoji && autocomplete.count >= 2 &&
        autocomplete.token_begin == 0 && autocomplete.token_end == 3;
    expect(one_character_hidden && two_character_visible,
           "composer_autocomplete_emoji_two_character_threshold");

    bool plus_one_found = false;
    const bool plus_one_ready = message_composer_clear(&composer) == MessageComposerResult::changed &&
        message_composer_insert_utf8(&composer, utf8_literal(":+1")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state);
    for (u32 i = 0; i < autocomplete.count; ++i) {
        const auto& suggestion = autocomplete.suggestions[i];
        plus_one_found = plus_one_found || utf8_equal(
            {suggestion.insertion, suggestion.insertion_bytes}, utf8_literal("\360\237\221\215 "));
    }
    expect(plus_one_ready && plus_one_found, "composer_autocomplete_plus_one_alias");

    bool troll_found = false;
    const bool troll_search_ready =
        message_composer_clear(&composer) == MessageComposerResult::changed &&
        message_composer_insert_utf8(&composer, utf8_literal(":troll")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) &&
        autocomplete.kind == ComposerAutocompleteKind::emoji;
    for (u32 index = 0; index < autocomplete.count; ++index) {
        const ComposerAutocompleteSuggestion& suggestion = autocomplete.suggestions[index];
        const Utf8View insertion{suggestion.insertion, suggestion.insertion_bytes};
        if (insertion.bytes == 5 && insertion.data[0] == static_cast<char>(0xF0) &&
            insertion.data[1] == static_cast<char>(0x9F) && insertion.data[2] == static_cast<char>(0xA7) &&
            insertion.data[3] == static_cast<char>(0x8C) && insertion.data[4] == ' ' &&
            utf8_equal(suggestion.label, utf8_literal("\xF0\x9F\xA7\x8C  :troll:"))) {
            troll_found = true;
        }
    }
    expect(troll_search_ready && troll_found && unicode_emoji_catalog().count == 3'780,
           "composer_autocomplete_unicode_16_troll_and_glyph_insertion");
    CustomEmojiRecord custom_emojis[3]{};
    u64 emoji_roles[1]{};
    state.emojis = custom_emojis; state.emoji_count = 3; state.emoji_roles = emoji_roles;
    state.selected_guild_id = 99;
    for (u32 i = 0; i < 3; ++i) {
        custom_emojis[i].id = 800 + i; custom_emojis[i].guild_id = i == 2 ? 100 : 99;
        custom_emojis[i].flags = i == 1 ? 0 : custom_emoji_available | custom_emoji_animated;
        const auto name = utf8_literal("maiblob");
        copy_bytes(custom_emojis[i].name, name.data, name.bytes); custom_emojis[i].name_bytes = static_cast<u8>(name.bytes);
    }
    message_composer_reset(&composer, 7, false);
    expect(message_composer_insert_utf8(&composer, utf8_literal(":maibl")) == MessageComposerResult::changed &&
        composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state) && autocomplete.count == 1 &&
        autocomplete.suggestions[0].emoji_id == 800 && autocomplete.suggestions[0].animated_emoji &&
        utf8_equal({autocomplete.suggestions[0].insertion, autocomplete.suggestions[0].insertion_bytes}, utf8_literal("<a:maiblob:800> ")),
        "custom_emoji_autocomplete_preserves_animated_identity_and_excludes_unavailable_or_other_guild_matches");

    // ':' ranking as the official client ranks it (owner's side-by-side for ":sk").
    constexpr u64 now_ms = 1'800'000'000'000ull;
    ReactionPreferenceStore preferences{};
    const auto build = [&](Utf8View query, const ReactionPreferenceStore* history) noexcept {
        return message_composer_restore(&composer, 7, query, query.bytes, query.bytes) &&
               composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state, nullptr,
                                           nullptr, history);
    };
    const auto remember = [&](ConstSlice<ReactionPreferenceRemoteUsage> message) noexcept {
        return reaction_preferences_apply_remote(&preferences, {}, {}, message, now_ms);
    };
    constexpr auto skull = utf8_literal("\xF0\x9F\x92\x80");
    const Utf8View no_history_order[] = {
        utf8_literal("flag_sk"), utf8_literal("skateboard"), utf8_literal("ski"), utf8_literal("skier"),
        utf8_literal("skull"), utf8_literal("skull_crossbones"), utf8_literal("skunk"), utf8_literal("asterisk"),
        utf8_literal("basket"), utf8_literal("basketball"), utf8_literal("cd"), utf8_literal("city_dusk"),
        utf8_literal("desktop"), utf8_literal("diving_mask")};
    expect(reaction_preferences_initialize(&preferences) && build(utf8_literal(":sk"), &preferences) &&
               emoji_suggestions_lead(autocomplete, no_history_order, 14) && emoji_insertions(autocomplete, skull) == 1 &&
               build(utf8_literal(":sk"), nullptr) && emoji_suggestions_lead(autocomplete, no_history_order, 14) &&
               autocomplete.count == kComposerAutocompleteCapacity,
           "emoji_autocomplete_without_history_ranks_word_then_prefix_then_substring_by_primary_name");

    const ReactionPreferenceRemoteUsage skull_used[] = {message_use(utf8_literal("skull"), 5, now_ms)};
    const Utf8View frecent_order[] = {
        utf8_literal("skull"), utf8_literal("flag_sk"), utf8_literal("skateboard"), utf8_literal("ski"),
        utf8_literal("skier"), utf8_literal("skull_crossbones"), utf8_literal("skunk"), utf8_literal("asterisk"),
        utf8_literal("basket"), utf8_literal("basketball"), utf8_literal("cd"), utf8_literal("city_dusk"),
        utf8_literal("desktop"), utf8_literal("diving_mask")};
    expect(remember({skull_used, 1}) && build(utf8_literal(":sk"), &preferences) &&
               emoji_suggestions_lead(autocomplete, frecent_order, 14) && emoji_insertions(autocomplete, skull) == 1 &&
               emoji_insertions(autocomplete, utf8_literal("\xF0\x9F\x87\xB8\xF0\x9F\x87\xB0")) == 1,
           "emoji_autocomplete_frequently_used_skull_leads_sk_as_in_the_official_client");

    // A substring match weighs 1 against a prefix's 2 and flag_sk's word match
    // 3: three recent uses (score 300) tie it with flag_sk, broken by name.
    // Settings keys below 2^22 are Unicode names, so the custom emoji takes a real snowflake.
    const auto whisky = utf8_literal("whisky");
    copy_bytes(custom_emojis[0].name, whisky.data, whisky.bytes);
    custom_emojis[0].name_bytes = static_cast<u8>(whisky.bytes);
    custom_emojis[0].id = 1'100'000'000'000'000'001ull;
    const ReactionPreferenceRemoteUsage whisky_used[] = {
        message_use(utf8_literal("1100000000000000001"), 3, now_ms)};
    const Utf8View custom_order[] = {utf8_literal("flag_sk"), whisky, utf8_literal("skateboard"),
                                     utf8_literal("ski"), utf8_literal("skier"), utf8_literal("skull")};
    const auto whisky_insertion = utf8_literal("<a:whisky:1100000000000000001> ");
    const bool unweighted_custom_absent =
        build(utf8_literal(":sk"), nullptr) &&
        emoji_insertions(autocomplete, {whisky_insertion.data, whisky_insertion.bytes - 1}) == 0;
    expect(unweighted_custom_absent && remember({whisky_used, 1}) && build(utf8_literal(":sk"), &preferences) &&
               emoji_suggestions_lead(autocomplete, custom_order, 6) &&
               autocomplete.suggestions[1].emoji_id == custom_emojis[0].id &&
               utf8_equal({autocomplete.suggestions[1].insertion, autocomplete.suggestions[1].insertion_bytes},
                          whisky_insertion),
           "emoji_autocomplete_weighs_a_frequent_custom_substring_match_by_its_frecency");

    // Skin tones: base emoji only; a toned key is its own entry and does not
    // weigh the base, which the official client scores by its own name.
    const ReactionPreferenceRemoteUsage toned_used[] = {message_use(utf8_literal("thumbsup_tone3"), 5, now_ms)};
    const u32 toned_index = emoji_catalog_find_glyph(emoji_shortcode_glyph(utf8_literal("thumbsup_tone3")));
    const Utf8View thumbs_order[] = {utf8_literal("thumbsdown"), utf8_literal("thumbsup")};
    bool untoned = build(utf8_literal(":thumbs"), nullptr) && emoji_suggestions_lead(autocomplete, thumbs_order, 2);
    for (u32 i = 0; i < autocomplete.count; ++i)
        for (u32 at = 0; at + 4 <= autocomplete.suggestions[i].insertion_bytes; ++at)
            untoned = untoned && !(bytes_equal(autocomplete.suggestions[i].insertion + at, "\xF0\x9F\x8F", 3) &&
                                   static_cast<u8>(autocomplete.suggestions[i].insertion[at + 3]) >= 0xBBu &&
                                   static_cast<u8>(autocomplete.suggestions[i].insertion[at + 3]) <= 0xBFu);
    const bool toned_key_maps = toned_index != ~0u && toned_used[0].emoji.valid() &&
                                toned_used[0].emoji.catalog_index == toned_index &&
                                toned_index != emoji_catalog_find_glyph(utf8_literal("\xF0\x9F\x91\x8D"));
    const bool toned_does_not_weigh_base = remember({toned_used, 1}) && build(utf8_literal(":thumbs"), &preferences) &&
                                           emoji_suggestions_lead(autocomplete, thumbs_order, 2);
    const ReactionPreferenceRemoteUsage base_used[] = {message_use(utf8_literal("thumbsup"), 2, now_ms)};
    const Utf8View base_order[] = {utf8_literal("thumbsup"), utf8_literal("thumbsdown")};
    expect(untoned && toned_key_maps && toned_does_not_weigh_base && remember({base_used, 1}) &&
               build(utf8_literal(":thumbs"), &preferences) && emoji_suggestions_lead(autocomplete, base_order, 2) &&
               emoji_insertions(autocomplete, utf8_literal("\xF0\x9F\x91\x8D")) == 1,
           "emoji_autocomplete_lists_base_emoji_and_scores_them_by_their_own_key");
    reaction_preferences_release(&preferences);
    composer_autocomplete_release(&autocomplete);
    expect(!autocomplete.storage.base && !autocomplete.suggestions && autocomplete.count == 0,
           "composer_autocomplete_storage_release");
}

void test_message_composer_controller() noexcept {
    MessageRecord message_records[8]{};
    u32 message_order[8]{};
    u8 message_text[512]{};
    MessageStore message_store{};
    u32 heights[8]{};
    u64 fenwick[8]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    char composer_text[128]{};
    MessageComposer composer{};
    MessageDraftRecord draft_records[4]{};
    u8 draft_text[128]{};
    MessageDraftStore drafts{};
    u8 request_text[kMessageComposerMaximumBytes]{};
    MessageComposerController controller{};
    const bool initialized =
        message_store_initialize(&message_store, Slice<MessageRecord>{message_records, 8},
                                 Slice<u32>{message_order, 8}, Slice<u8>{message_text, 512}) &&
        ui_variable_list_initialize(&list, Slice<u32>{heights, 8}, Slice<u64>{fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &message_store, &list, 100, 100, true) &&
        message_composer_initialize(&composer, Slice<char>{composer_text, 128}) &&
        message_draft_store_initialize(&drafts, Slice<MessageDraftRecord>{draft_records, 4},
                                       Slice<u8>{draft_text, 128}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline,
                                               Slice<u8>{request_text, kMessageComposerMaximumBytes});
    message_composer_reset(&composer, 7, false);
    expect(initialized, "message_composer_controller_initialize");
    {
        static u8 untouched[kMessageComposerMaximumBytes];
        for (u8& byte : untouched)
            byte = 0x5A;
        MessageComposerController sparse{};
        bool kept = message_composer_controller_initialize(
            &sparse, &composer, &drafts, &timeline, Slice<u8>{untouched, kMessageComposerMaximumBytes});
        for (const u8 byte : untouched)
            kept = kept && byte == 0x5A;
        expect(kept && sparse.initialized && sparse.request_text == untouched,
               "message_composer_controller_initialize_leaves_request_bank_untouched");
    }

    const bool channel_seven_draft =
        message_composer_insert_utf8(&composer, utf8_literal("alpha")) == MessageComposerResult::changed &&
        message_composer_controller_commit_draft(&controller).ok();
    const MessageComposerControllerResult channel_eight =
        message_composer_controller_select_channel(&controller, 8);
    const bool channel_eight_draft =
        channel_eight.ok() && composer.channel_id == 8 && composer.bytes == 0 &&
        message_composer_insert_utf8(&composer, utf8_literal("beta")) == MessageComposerResult::changed &&
        message_composer_move(&composer, MessageComposerMove::previous, true) ==
            MessageComposerResult::changed &&
        message_composer_controller_commit_draft(&controller).ok();
    const MessageComposerControllerResult restored_seven =
        message_composer_controller_select_channel(&controller, 7);
    const bool seven_restored = restored_seven.ok() && composer.channel_id == 7 &&
                                utf8_equal(message_composer_text(composer), utf8_literal("alpha")) &&
                                composer.anchor == 5 && composer.caret == 5;
    const MessageComposerControllerResult restored_eight =
        message_composer_controller_select_channel(&controller, 8);
    expect(channel_seven_draft && channel_eight_draft && seven_restored && restored_eight.ok() &&
               utf8_equal(message_composer_text(composer), utf8_literal("beta")) &&
               composer.anchor == 4 && composer.caret == 3 &&
               drafts.count == 2,
           "message_composer_controller_per_channel_draft_restore");

    expect(message_composer_controller_set_reply(&controller, 77).ok(),
           "message_composer_controller_reply_owner_capture");
    const MessageComposerControllerResult queued =
        message_composer_controller_queue_send(&controller, true, 11, 1'725'012'296'789ull, 100, 77);
    MessageComposerSubmit submit{};
    const MessageComposerControllerResult prepared =
        message_composer_controller_prepare_submit(&controller, 101, &submit);
    const bool request_owned = queued.ok() && queued.nonce != 0 && composer.bytes == 0 &&
                               drafts.count == 1 && prepared.ok() && submit.nonce == queued.nonce &&
                               submit.channel_id == 8 && submit.reply_message_id == 77 &&
                               utf8_equal(submit.content, utf8_literal("beta")) &&
                               message_composer_controller_mark_submitted(&controller, submit.nonce).ok();
    const bool switched_while_inflight =
        message_composer_controller_select_channel(&controller, 7).ok() &&
        message_timeline_reset(&timeline) &&
        utf8_equal(message_composer_text(composer), utf8_literal("alpha"));
    const MessageComposerControllerResult http = message_composer_controller_observe_http(
        &controller, 200, MessageSendHttpDisposition::success, 110);
    const MessageSendObserveResult gateway = message_send_tracker_observe_gateway(
        message_composer_controller_send_tracker(&controller), &timeline, queued.nonce, 8, 11, 900, 115);
    MessageSendCompletion completion{};
    const bool completed = message_composer_controller_take_completion(&controller, &completion);
    expect(request_owned && switched_while_inflight && http.ok() && gateway.ok() && gateway.completed &&
               completed && completion.kind == MessageSendCompletionKind::accepted &&
               completion.nonce == queued.nonce && controller.request_count == 0 &&
               utf8_equal(message_composer_text(composer), utf8_literal("alpha")),
           "message_composer_controller_send_survives_channel_switch");

    const MessageComposerControllerResult deferred_send =
        message_composer_controller_queue_send(&controller, true, 11, 1'725'012'296'790ull, 200);
    const MessageComposerControllerResult deferred_prepare =
        message_composer_controller_prepare_submit(&controller, 201, &submit);
    const bool marked =
        deferred_prepare.ok() &&
        message_composer_controller_mark_submitted(&controller, deferred_prepare.nonce).ok();
    const MessageComposerControllerResult deferred = message_composer_controller_observe_http(
        &controller, 429, MessageSendHttpDisposition::retry_wait, 210, 500);
    const u64 deferred_wake = message_composer_controller_next_wake_at(controller);
    MessageComposerSubmit early_submit{};
    const MessageComposerControllerResult early =
        message_composer_controller_prepare_submit(&controller, 499, &early_submit);
    const MessageComposerControllerResult due =
        message_composer_controller_prepare_submit(&controller, 500, &submit);
    const bool second_mark =
        due.ok() && message_composer_controller_mark_submitted(&controller, due.nonce).ok();
    const MessageComposerControllerResult failed =
        second_mark ? message_composer_controller_observe_http(
                          &controller, 400, MessageSendHttpDisposition::terminal_failure, 510)
                    : MessageComposerControllerResult{};
    const MessageComposerControllerResult retried =
        failed.ok() ? message_composer_controller_retry(&controller, deferred_send.nonce, 520)
                    : MessageComposerControllerResult{};
    const MessageComposerControllerResult retry_submit =
        retried.ok() ? message_composer_controller_prepare_submit(&controller, 521, &submit)
                     : MessageComposerControllerResult{};
    const bool retry_marked =
        retry_submit.ok() &&
        message_composer_controller_mark_submitted(&controller, retry_submit.nonce).ok();
    bool timeout_changed = false;
    const u64 timeout_at = 520 + kMessageSendTrackerTimeoutMs;
    const bool timeout_tick =
        retry_marked && message_composer_controller_tick(&controller, timeout_at, &timeout_changed);
    completion = MessageSendCompletion{};
    const bool timeout_completion =
        timeout_tick && message_composer_controller_take_completion(&controller, &completion);
    const bool timeout_receipt_owned = controller.inflight_nonce == deferred_send.nonce &&
        message_composer_controller_retry(&controller, deferred_send.nonce, timeout_at + 1).error ==
            MessageComposerControllerError::busy &&
        message_composer_controller_observe_http(&controller, 0,
            MessageSendHttpDisposition::terminal_failure, timeout_at + 1).ok();
    const MessageComposerControllerResult retry_after_timeout =
        timeout_completion
            ? message_composer_controller_retry(&controller, deferred_send.nonce, timeout_at + 1)
            : MessageComposerControllerResult{};
    const MessageComposerControllerResult discard_prepare =
        retry_after_timeout.ok()
            ? message_composer_controller_prepare_submit(&controller, timeout_at + 2, &submit)
            : MessageComposerControllerResult{};
    const bool discard_marked =
        discard_prepare.ok() &&
        message_composer_controller_mark_submitted(&controller, discard_prepare.nonce).ok();
    const MessageComposerControllerResult discard_failure =
        discard_marked ? message_composer_controller_observe_http(
                             &controller, 503, MessageSendHttpDisposition::terminal_failure, timeout_at + 3)
                       : MessageComposerControllerResult{};
    const MessageComposerControllerResult discarded =
        discard_failure.ok() ? message_composer_controller_discard_failed(&controller, deferred_send.nonce)
                             : MessageComposerControllerResult{};
    expect(deferred_send.ok() && marked && deferred.ok() && deferred_wake == 500 &&
               early.error == MessageComposerControllerError::not_found && due.ok() && second_mark &&
               failed.ok() && retried.ok() && retry_submit.ok() && retry_marked && timeout_tick &&
               timeout_changed && timeout_completion && timeout_receipt_owned &&
               completion.kind == MessageSendCompletionKind::expired && retry_after_timeout.ok() &&
               discard_prepare.ok() && discard_marked && discard_failure.ok() && discarded.ok() &&
               discarded.invalidation.structural && controller.request_count == 0 &&
               send_tracker_active(controller.sends) == 0 &&
               message_store_find(message_store, deferred_send.nonce).error == MessageStoreError::not_found,
           "message_composer_controller_timeout_retry_and_discard");
    char profile_text[128]{};
    MessageComposer profile_composer{};
    message_composer_initialize(&profile_composer,{profile_text,128});
    expect(message_composer_restore(&profile_composer,0,utf8_literal("test test test"),14,14) &&
        text_input_ready(message_composer_input_target(&profile_composer)) && !message_composer_can_send(profile_composer,true),
        "profile_message_snapshot_edits_without_a_channel_but_cannot_send_until_resolved");
    message_composer_reset(&profile_composer,7,false);
    message_composer_insert_utf8(&composer,utf8_literal("Keep my DM draft"));
    message_composer_controller_commit_draft(&controller);
    message_composer_insert_utf8(&profile_composer,utf8_literal("test test test"));
    const auto profile_send = message_composer_controller_queue_send(&controller,true,11,1'725'012'300'000ull,40000,0,{}, {},&profile_composer);
    expect(profile_send.ok() && !profile_composer.bytes && drafts.count == 1 &&
        utf8_equal(message_composer_text(composer),utf8_literal("Keep my DM draft")),
        "profile_send_uses_shared_outbox_without_overwriting_dm_draft");
    expect(message_composer_controller_prepare_submit(&controller,40001,&submit).ok() &&
        submit.channel_id == 7 && utf8_equal(submit.content,utf8_literal("test test test")),
        "profile_send_preserves_typed_content_and_target");

}

} // namespace maicord

#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_send_response.hpp"
#include "maicord/model/invite_card_cache.hpp"
#include "maicord/model/message_reconcile.hpp"
#include "maicord/protocol/message_interaction.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/message_component_media.hpp"
#include "maicord/ui/message_media_policy.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/message_system_text.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/ui/navigation_view.hpp"

// Message content wave: system rows, received spoilers and explicit media,
// components and interactions, modals, ephemeral rows and invite cards.
namespace maicord {
namespace {

using Check = void (*)(bool, const char*) noexcept;

Utf8View span_text(const MessageSystemText& text, MessageSystemSpanKind kind) noexcept {
    for (u32 i = 0; i < text.span_count; ++i)
        if (text.spans[i].kind == kind)
            return {text.text.data + text.spans[i].offset, text.spans[i].bytes};
    return {};
}

void test_system_text(Check check) noexcept {
    char storage[kMessageSystemTextMaximumBytes];
    MessageSystemText text{};
    MessageSystemTextInput input{};
    input.author = utf8_literal("Ada");
    input.target = utf8_literal("Lin");

    check(!message_type_is_system(0) && !message_type_is_system(19) && !message_type_is_system(20) &&
              !message_type_is_system(21) && !message_type_is_system(23) && message_type_is_system(7) &&
              message_type_is_system(6) && message_type_is_system(46) && message_type_is_system(0xFFF0),
          "system_types_exclude_authored_rows");

    input.type = message_type_recipient_add;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada added Lin to the group.")) && text.icon == UiIcon::arrow_right &&
              utf8_equal(span_text(text, MessageSystemSpanKind::target), utf8_literal("Lin")),
          "system_recipient_add_names_both_users");
    input.type = message_type_recipient_remove;
    input.same_user = true;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada left the group.")) && text.tone == MessageSystemTone::negative,
          "system_recipient_remove_self_is_leave");
    input.same_user = false;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada removed Lin from the group.")),
          "system_recipient_remove_other");

    input.type = message_type_call;
    input.call_present = true;
    input.call_ended = true;
    input.call_participated = true;
    input.timestamp_ms = 1'000'000;
    input.call_ended_ms = 1'000'000 + 5 * 60'000 + 10'000;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada started a call that lasted 5 minutes.")) && text.icon == UiIcon::phone,
          "system_call_duration");
    input.call_participated = false;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("You missed a call from Ada that lasted 5 minutes.")) &&
              text.icon == UiIcon::phone_off,
          "system_call_missed");
    input.call_ended = false;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada started a call.")),
          "system_call_ongoing");

    input.type = message_type_channel_pinned_message;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada pinned a message to this channel. See all pinned messages.")) &&
              utf8_equal(span_text(text, MessageSystemSpanKind::message), utf8_literal("a message")) &&
              utf8_equal(span_text(text, MessageSystemSpanKind::pins), utf8_literal("pinned messages")) &&
              utf8_equal(span_text(text, MessageSystemSpanKind::author), utf8_literal("Ada")) && text.icon == UiIcon::pin,
          "system_pin_actions");

    input.type = message_type_user_join;
    // Snowflake timestamps select one of thirteen own-words welcome lines.
    input.message_id = 5ull << 22;
    check(message_system_join_line_index(input.message_id) == 5 &&
              message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Look who's here: Ada!")),
          "system_join_line_by_snowflake");
    bool all_named = true;
    for (u64 i = 0; i < kMessageSystemJoinLineCount; ++i) {
        input.message_id = i << 22;
        all_named = all_named && message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
                    utf8_equal(span_text(text, MessageSystemSpanKind::author), utf8_literal("Ada"));
    }
    check(all_named, "system_join_lines_name_the_member");

    input.type = message_type_guild_boost;
    input.content = utf8_literal("3");
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada just boosted the server 3 times!")) && text.icon == UiIcon::boost,
          "system_boost_count");
    input.type = message_type_guild_boost_tier_2;
    input.guild = utf8_literal("Cafe");
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada just boosted the server! Cafe reached Level 2!")),
          "system_boost_tier");

    input.type = message_type_thread_created;
    input.content = utf8_literal("Plans");
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada started a thread: Plans")) &&
              utf8_equal(span_text(text, MessageSystemSpanKind::thread), utf8_literal("Plans")),
          "system_thread_created_link");
    input.type = message_type_channel_name_change;
    input.forum_post = true;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada changed the post title: Plans")),
          "system_forum_post_rename");
    input.forum_post = false;
    input.type = message_type_stage_topic;
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada changed the Stage topic: Plans")),
          "system_stage_topic");

    input.type = message_type_poll_result;
    input.poll_question = utf8_literal("Lunch?");
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Ada's poll Lunch? has closed.")) &&
              utf8_equal(span_text(text, MessageSystemSpanKind::message), utf8_literal("Lunch?")),
          "system_poll_result_line_links_the_poll");

    input.type = 0xFFF0;
    input.content = utf8_literal("Blocked.");
    check(message_system_text_build(input, {storage, sizeof(storage)}, &text) &&
              utf8_equal(text.text, utf8_literal("Blocked.")) && text.icon == UiIcon::info,
          "system_unknown_type_uses_content");

    char duration[32];
    check(utf8_equal({duration, message_system_duration(5'000, {duration, 32})}, utf8_literal("a few seconds")) &&
              utf8_equal({duration, message_system_duration(3'600'000, {duration, 32})}, utf8_literal("an hour")) &&
              utf8_equal({duration, message_system_duration(3 * 86'400'000ull, {duration, 32})}, utf8_literal("3 days")),
          "system_durations");
    char tiny[8];
    check(!message_system_text_build(input, {tiny, 4}, &text), "system_text_rejects_small_output");
}

struct MediaPolicyFixture {
    MessageMediaPolicyResult result;
    mutable MessageMediaOwner last_owner;
    mutable u32 calls;
};
MessageMediaPolicyResult query_media_policy(const void* context, const MessageMediaOwner& owner) noexcept {
    const auto& fixture = *static_cast<const MediaPolicyFixture*>(context);
    fixture.last_owner = owner;
    ++fixture.calls;
    return fixture.result;
}

void test_media_policy(Check check) noexcept {
    MessageMediaReveals reveals{};
    const u64 key = message_media_reveal_key(42, MessageMediaSource::attachment, 0);
    check(key != message_media_reveal_key(42, MessageMediaSource::embed, 0) &&
              key != message_media_reveal_key(42, MessageMediaSource::attachment, 1) &&
              key != message_media_reveal_key(42, MessageMediaSource::attachment, 0, true),
          "media_reveal_keys_distinguish_items");
    check(message_media_obscure(reveals, key, true, false, MessageExplicitFilter::show, false) ==
                  MessageMediaObscure::spoiler &&
              message_media_obscure(reveals, key, true, false, MessageExplicitFilter::show, true) ==
                  MessageMediaObscure::none &&
              message_media_obscure(reveals, key, false, true, MessageExplicitFilter::blur, false) ==
                  MessageMediaObscure::explicit_content &&
              message_media_obscure(reveals, key, true, true, MessageExplicitFilter::block, false) ==
                  MessageMediaObscure::blocked &&
              message_media_obscure(reveals, key, false, true, MessageExplicitFilter::show, false) ==
                  MessageMediaObscure::none,
          "media_obscure_states");
    check(message_media_reveal(&reveals, key) && !message_media_reveal(&reveals, key) &&
              message_media_obscure(reveals, key, true, true, MessageExplicitFilter::blur, false) ==
                  MessageMediaObscure::none &&
              message_media_obscure(reveals, key, true, true, MessageExplicitFilter::block, false) ==
                  MessageMediaObscure::blocked,
          "media_reveal_is_sticky_but_block_wins");
    check(message_media_obscure(reveals, key, false, true, MessageExplicitFilter::blur, false, false) ==
              MessageMediaObscure::explicit_content,
          "media_explicit_confirmation_revokes_prior_reveal");

    AccountContentPolicy account{};
    constexpr MessageMediaContext contexts[]{MessageMediaContext::guild, MessageMediaContext::friend_dm,
                                             MessageMediaContext::other_dm};
    constexpr MessageExplicitFilter adult[3][3]{
        {MessageExplicitFilter::show, MessageExplicitFilter::show, MessageExplicitFilter::show},
        {MessageExplicitFilter::show, MessageExplicitFilter::show, MessageExplicitFilter::block},
        {MessageExplicitFilter::show, MessageExplicitFilter::block, MessageExplicitFilter::block}};
    constexpr MessageExplicitFilter teen[3][3]{
        {MessageExplicitFilter::blur, MessageExplicitFilter::blur, MessageExplicitFilter::blur},
        {MessageExplicitFilter::blur, MessageExplicitFilter::blur, MessageExplicitFilter::block},
        {MessageExplicitFilter::blur, MessageExplicitFilter::block, MessageExplicitFilter::block}};
    bool defaults = true, overrides = true;
    for (u32 legacy = 0; legacy < 3; ++legacy)
        for (u32 context = 0; context < 3; ++context) {
            account.eligibility = AccountContentEligibility::unknown;
            defaults &= message_explicit_filter_resolve(contexts[context], 0, legacy, account) ==
                        adult[legacy][context];
            account.eligibility = AccountContentEligibility::restricted;
            defaults &= message_explicit_filter_resolve(contexts[context], 9, legacy, account) ==
                        teen[legacy][context];
            account.eligibility = AccountContentEligibility::unknown;
            account.flags = account_content_teen_defaults;
            defaults &= message_explicit_filter_resolve(contexts[context], 0, legacy, account) ==
                        (context == 2 ? MessageExplicitFilter::block : MessageExplicitFilter::blur);
            for (u64 value = 1; value <= 3; ++value)
                overrides &= message_explicit_filter_resolve(contexts[context], value, legacy, account) ==
                             (value == 1   ? MessageExplicitFilter::show
                              : value == 2 ? MessageExplicitFilter::blur
                                           : MessageExplicitFilter::block);
            account.flags = 0;
        }
    check(defaults && overrides, "media_explicit_settings_adult_teen_region_and_legacy_table");

    MessageScene scene{};
    MessageRecord record{};
    record.id = 42;
    record.channel_id = 17;
    record.author_id = 19;
    const MessageMediaOwner normal = message_media_owner(record, nullptr);
    check(normal.channel_id == 17 && normal.author_id == 19 && !normal.forwarded &&
              message_scene_media_policy(scene, normal).filter == MessageExplicitFilter::show &&
              message_scene_media_policy(scene, normal).explicit_reveal_allowed,
          "media_owner_and_unbound_scene_show_media");
    MessageRichRecord forward{};
    forward.reference.type = 1;
    forward.reference.channel_id = 71;
    forward.reference.message_id = 73;
    forward.reference.author_id = 999; // A reference label is not cached source-author authority.
    const MessageMediaOwner owner = message_media_owner(record, &forward);
    record.type = message_type_channel_pinned_message;
    const auto system_owner = message_media_owner(record, &forward);
    record.type = 0;
    forward.system.message_type = message_type_channel_pinned_message;
    const auto rich_system_owner = message_media_owner(record, &forward);
    forward.system.message_type = 0;
    check(!system_owner.forwarded && system_owner.author_id == 19 && !rich_system_owner.forwarded &&
              rich_system_owner.author_id == 19,
          "media_system_reference_action_is_not_a_forward");
    MediaPolicyFixture fixture{{MessageExplicitFilter::blur, true}, {}, 0};
    MessageMediaPolicy policy{&fixture, query_media_policy, 1};
    check(owner.forwarded && owner.channel_id == 17 && !owner.author_id && owner.source_channel_id == 71 &&
              owner.source_message_id == 73 && message_scene_set_media_policy(&scene, policy) &&
              !message_scene_set_media_policy(&scene, policy) &&
              message_scene_media_obscure(scene, owner, 0, key, false, true) ==
                  MessageMediaObscure::explicit_content &&
              fixture.last_owner.channel_id == 17 && fixture.last_owner.source_message_id == 73 &&
              fixture.calls == 1,
          "media_forward_policy_retains_destination_and_queries_source_without_plugin_identity");
    message_scene_reveal_media(&scene, key);
    check(message_scene_media_obscure(scene, owner, 0, key, false, true) == MessageMediaObscure::none &&
              message_scene_media_obscure(
                  scene, owner, 0, message_media_reveal_key(42, MessageMediaSource::attachment, 0, true),
                  false, true) == MessageMediaObscure::explicit_content,
          "media_snapshot_reveal_is_independent");
    fixture.result.explicit_reveal_allowed = false;
    ++policy.revision;
    scene.dirty = false;
    check(message_scene_set_media_policy(&scene, policy) && scene.dirty &&
              message_scene_media_obscure(scene, owner, 0, key, false, true) ==
                  MessageMediaObscure::explicit_content,
          "media_policy_revision_invalidates_geometry_and_revokes_explicit_reveal");
    for (u32 i = 1; i <= kMessageMediaRevealMaximum; ++i)
        message_media_reveal(&reveals,
                             message_media_reveal_key(1000 + i, MessageMediaSource::component, i));
    check(!message_media_revealed(reveals, key) && reveals.count == kMessageMediaRevealMaximum,
          "media_reveal_ring_is_bounded");
    check(utf8_equal(message_media_obscure_label(MessageMediaObscure::spoiler), utf8_literal("Spoiler")) &&
              ui_rect_has_area(message_media_obscure_pill({0, 0, 300, 200})) &&
              !ui_rect_has_area(message_media_obscure_pill({0, 0, 30, 20})),
          "media_obscure_label_and_pill");
}

// Decodes a Gateway MESSAGE_CREATE through the account transport (pack and
// unpack) into the event, as the client receives it.
struct DecodeFixture {
    u32 structurals[4096];
    char packed_text[16384];
    u64 ids[100];
    GatewayMessageComponent components[kGatewayMessageComponentMaximum];
    GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField fields[kGatewayMessageEmbedFieldMaximum];
    GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum];
    GatewayMessageNamedUser users[8];
};
DecodeFixture decode_fixture;

bool decode_create(Utf8View payload, GatewayMessageEvent* message, u64 self_user = 0) noexcept {
    auto& f = decode_fixture;
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {f.components, kGatewayMessageComponentMaximum};
    scratch.embeds = {f.embeds, kGatewayMessageEmbedMaximum};
    scratch.embed_fields = {f.fields, kGatewayMessageEmbedFieldMaximum};
    scratch.attachments = {f.attachments, kGatewayMessageAttachmentMaximum};
    return gateway_decode_envelope(payload, {f.structurals, 4096}, 32, &envelope).ok() &&
           gateway_client_route_dispatch(envelope, {f.structurals, 4096}, 32, {f.packed_text, sizeof(f.packed_text)},
                                         {f.ids, 100}, self_user, &routed, &packed)
               .ok() &&
           gateway_client_event_as_message(routed, packed, scratch, message) &&
           gateway_client_event_message_system_target(routed, packed, message, &f.users[0]);
}

struct RichFixture {
    MessageRichRecord records[2];
    u16 order[2];
    u8 text[16384];
    MessageRichStore store;
};
RichFixture rich_fixture;

void test_components(Check check) noexcept {
    const auto payload = utf8_literal(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{
        "id":"100","channel_id":"7","author":{"id":"9","username":"Bot","bot":true},"content":"",
        "timestamp":"2026-09-05T10:00:00Z","flags":32768,"type":0,
        "components":[
          {"type":1,"components":[
            {"type":2,"style":1,"label":"Accept","custom_id":"ok","emoji":{"id":"55","name":"yes","animated":true}},
            {"type":2,"style":4,"label":"Delete","custom_id":"rm","disabled":true},
            {"type":2,"style":5,"label":"Docs","url":"https://example.com/docs","emoji":{"name":"📖"}}]},
          {"type":1,"components":[
            {"type":3,"custom_id":"pick","placeholder":"Choose a role","min_values":1,"max_values":2,"options":[
              {"label":"Red","value":"r","description":"Warm","default":true},
              {"label":"Blue","value":"b","emoji":{"name":"🔵"}}]}]},
          {"type":9,"components":[{"type":10,"content":"Section text"}],
           "accessory":{"type":11,"media":{"url":"https://example.com/a.png","proxy_url":"https://media.discordapp.net/a.png","width":170,"height":85},"spoiler":true}},
          {"type":12,"items":[{"media":{"url":"https://example.com/1.jpg"}},{"media":{"url":"https://example.com/2.webp"},"description":"Two","spoiler":true}]},
          {"type":14,"divider":false,"spacing":2},
          {"type":13,"file":{"url":"attachment://notes.txt"},"name":"notes.txt"},
          {"type":5,"custom_id":"who"}]}})");
    GatewayMessageEvent message{};
    const bool decoded = decode_create(payload, &message);
    const auto* c = decode_fixture.components;
    check(decoded && message.components.count == 17, "components_decode_tree_with_pseudo_children");
    if (!decoded || message.components.count != 17)
        return;
    // BFS order: roots 0-6, then their children.
    check(c[0].type == 1 && c[2].type == 9 && c[3].type == 12 && c[4].type == 14 && c[5].type == 13 &&
              c[6].type == 5 && c[4].style == 2 && !(c[4].flags & gateway_message_component_divider) &&
              utf8_equal(c[5].label, utf8_literal("notes.txt")) && utf8_equal(c[5].url, utf8_literal("attachment://notes.txt")),
          "components_roots_separator_and_file");
    const GatewayMessageComponent* accept = nullptr;
    const GatewayMessageComponent* select = nullptr;
    const GatewayMessageComponent* thumbnail = nullptr;
    u32 options = 0, gallery_items = 0, spoiler_items = 0;
    for (u32 i = 0; i < message.components.count; ++i) {
        if (c[i].type == 2 && utf8_equal(c[i].custom_id, utf8_literal("ok")))
            accept = &c[i];
        if (c[i].type == 3)
            select = &c[i];
        if (c[i].type == 11)
            thumbnail = &c[i];
        if (c[i].type == gateway_component_select_option) {
            ++options;
            check(c[c[i].parent].type == 3, "components_options_parented_to_select");
        }
        if (c[i].type == gateway_component_gallery_item) {
            ++gallery_items;
            spoiler_items += (c[i].flags & gateway_message_component_spoiler) ? 1u : 0u;
        }
    }
    check(accept && accept->emoji_id == 55 && utf8_equal(accept->emoji_name, utf8_literal("yes")) &&
              (accept->flags & gateway_message_component_emoji_animated) && accept->style == 1,
          "components_button_emoji");
    check(select && utf8_equal(select->content, utf8_literal("Choose a role")) && select->min_values == 1 &&
              select->max_values == 2 && options == 2,
          "components_select_placeholder_and_bounds");
    check(thumbnail && c[thumbnail->parent].type == 9 && thumbnail->order == 0xFFFFu &&
              utf8_equal(thumbnail->url, utf8_literal("https://media.discordapp.net/a.png")) &&
              (thumbnail->flags & gateway_message_component_spoiler),
          "components_section_accessory_prefers_proxy");
    check(gallery_items == 2 && spoiler_items == 1, "components_gallery_items");

    auto& r = rich_fixture;
    bool ok = message_rich_store_initialize(&r.store, {r.records, 2}, {r.order, 2}, {r.text, sizeof(r.text)}) &&
              message_rich_store_apply(&r.store, 100, 1, message).ok();
    const auto* rich = message_rich_store_find(r.store, 100);
    check(ok && rich && rich->component_count == 17, "components_stored");
    if (!rich)
        return;
    const MessageRichComponent* stored_option = nullptr;
    for (u32 i = 0; i < rich->component_count; ++i)
        if (rich->components[i].type == gateway_component_select_option &&
            (rich->components[i].flags & gateway_message_component_default))
            stored_option = &rich->components[i];
    check(stored_option && utf8_equal(message_rich_store_string(r.store, stored_option->custom_id), utf8_literal("r")) &&
              utf8_equal(message_rich_store_string(r.store, stored_option->content), utf8_literal("Warm")),
          "components_option_value_and_description_stored");
    u32 interactive = 0, links = 0;
    for (u32 i = 0; i < rich->component_count; ++i) {
        interactive += message_component_interactive(rich->components[i]) ? 1u : 0u;
        links += message_component_is_link(rich->components[i]) ? 1u : 0u;
        if (message_component_kind(rich->components[i]) == MessageComponentKind::hidden)
            check(rich->components[i].type == gateway_component_select_option, "components_only_options_hidden");
    }
    check(interactive == 4 && links == 1, "components_interactive_buttons_and_selects");
    MessageComponentMedia media{};
    check(thumbnail && message_component_media_resolve(r.store, rich->components[thumbnail - c], &media) &&
              media.key.kind == MediaAssetKind::profile_image && media.format == static_cast<u8>(MediaAttachmentFormat::png),
          "components_thumbnail_media_key");
    check(message_component_media_format(utf8_literal("https://x/y.JPEG?size=1")) == MediaAttachmentFormat::jpeg,
          "components_media_format_by_extension");

    // Layout: buttons share a row, the select is bounded, the section keeps
    // its accessory lane and the gallery lays out a two-item grid.
    MessageRichItemLayout items = message_rich_estimate_item_layout(&r.store, rich, 600.0f);
    const float row_height = items.heights[kMessageRichComponentItemOffset + 0];
    const float section_height = items.heights[kMessageRichComponentItemOffset + 2];
    const float gallery_height = items.heights[kMessageRichComponentItemOffset + 3];
    check(row_height == kMessageComponentButtonHeight && section_height == kMessageComponentThumbnailExtent &&
              gallery_height > 0 && items.component_widths[1] <= kMessageComponentSelectMaximumWidth + 0.5f,
          "components_layout_heights");
    const UiRect left = message_component_gallery_cell(2, 0, 400), right = message_component_gallery_cell(2, 1, 400);
    check(left.right <= right.left && left.top == right.top && ui_rect_width(left) == ui_rect_width(right) &&
              message_component_gallery_height(1, 400) == 225.0f &&
              ui_rect_height(message_component_gallery_cell(3, 0, 300)) ==
                  message_component_gallery_height(3, 300),
          "components_gallery_grid");
    // Mosaic caps: pairs at 280, the lead of a larger gallery is count % 3
    // items, then rows of three squares 4 apart.
    const UiRect six = message_component_gallery_cell(6, 3, 600), seven_lead = message_component_gallery_cell(7, 0, 600),
                 seven_first = message_component_gallery_cell(7, 1, 600);
    const float seven_height = message_component_gallery_height(7, 600),
                seven_bottom = message_component_gallery_cell(7, 6, 600).bottom;
    check(message_component_gallery_height(2, 600) == 280.0f && six.top > 0.0f && six.left == 0.0f &&
              ui_rect_width(seven_lead) == 600.0f && ui_rect_height(seven_lead) == 280.0f &&
              seven_first.top == 284.0f && seven_first.left == 0.0f &&
              ui_rect_width(seven_first) - ui_rect_height(seven_first) < 0.01f &&
              ui_rect_height(seven_first) - ui_rect_width(seven_first) < 0.01f && seven_height - seven_bottom < 0.01f &&
              seven_bottom - seven_height < 0.01f,
          "components_gallery_mosaic_follows_official_layout");
    // A spacing-2 separator without a divider adds 8 above and below; a
    // small divider is a 1-DIP rule between the parent's 8-DIP gaps.
    MessageRichComponent small_divider{};
    small_divider.type = gateway_component_separator;
    small_divider.style = 1;
    small_divider.flags = gateway_message_component_divider;
    check(rich->components[4].type == gateway_component_separator &&
              message_component_leaf_height(rich->components[4]) == 16.0f &&
              message_component_leaf_height(small_divider) == 1.0f,
          "components_separator_spacing");
    u32 accessory = 0;
    for (u32 i = 0; i < rich->component_count; ++i)
        if (rich->components[i].type == 11)
            accessory = i;
    const UiLayoutStyle accessory_style = message_component_style(*rich, accessory, items);
    check(accessory_style.horizontal_alignment == UiLayoutAlignment::end, "components_section_accessory_trailing");
    const auto primary = message_component_button_colors(kDefaultTheme, 1, false);
    const auto danger = message_component_button_colors(kDefaultTheme, 4, true);
    const auto secondary = message_component_button_colors(kDefaultTheme, 2, false);
    const auto premium = message_component_button_colors(kDefaultTheme, 6, false);
    check(primary.fill.red == kDefaultTheme.tokens.emphasis.fill.red && danger.fill.alpha < 1.0f &&
              bytes_equal(&premium.fill, &primary.fill, sizeof(Color)) &&
              bytes_equal(&secondary.fill, &kDefaultTheme.tokens.fill.muted, sizeof(Color)) &&
              secondary.hover.alpha > secondary.fill.alpha &&
              bytes_equal(&secondary.border, &kDefaultTheme.tokens.border.subtle, sizeof(Color)),
          "components_button_colors_follow_tokens");
    // V2 messages stack top-level components 8 apart, legacy rows 6.
    static MessageRichRecord variant{};
    copy_value(&variant, *rich);
    variant.components_v2 = false;
    check(rich->components_v2 && message_component_root_gap(rich) == kMessageComponentGap &&
              message_component_root_gap(&variant) == kMessageComponentLegacyRootGap &&
              message_component_structure(rich) != message_component_structure(&variant),
          "components_root_gap_follows_the_v2_flag");
    // Media sizes travel with the item: a lone gallery item keeps its aspect.
    const UiRect wide = message_component_gallery_cell(1, 0, 400, 0.25f),
                 tall = message_component_gallery_cell(1, 0, 400, 2.0f);
    check(message_component_media_aspect(rich->components[accessory]) == 0.5f && ui_rect_width(wide) == 400.0f &&
              ui_rect_height(wide) == 100.0f && ui_rect_height(tall) == 350.0f && ui_rect_width(tall) == 175.0f,
          "components_single_gallery_item_keeps_its_aspect");
    // A failed button's error line grows its action row by 28.
    u32 accept_index = 0;
    for (u32 i = 0; i < rich->component_count; ++i)
        if (rich->components[i].type == gateway_component_button && rich->components[i].style == 1)
            accept_index = i;
    const u32 host = message_component_error_host(*rich, accept_index);
    MessageRichItemLayout failed_items = items;
    message_component_measure_tree(*rich, &failed_items, 600.0f, host);
    message_component_measure_tree(*rich, &items, 600.0f);
    check(host == rich->components[accept_index].parent &&
              failed_items.heights[kMessageRichComponentItemOffset + host] ==
                  items.heights[kMessageRichComponentItemOffset + host] + kMessageComponentErrorHeight &&
              message_component_error_host(*rich, host) == kMessageComponentNone,
          "components_failed_control_grows_its_row_by_the_error_line");
    // A spoiler container hides everything inside it and takes the click.
    variant.components[host].type = gateway_component_container;
    variant.components[host].flags = gateway_message_component_spoiler;
    check(message_component_spoiler_container(variant, accept_index) == host &&
              message_component_spoiler_container(*rich, accept_index) == kMessageComponentNone &&
              message_component_hit_target(variant.components[host]) &&
              !message_component_hit_target(rich->components[host]),
          "components_spoiler_container_conceals_its_children");
    message_rich_store_release(&r.store);
}

void test_system_records(Check check) noexcept {
    auto& r = rich_fixture;
    bool ok = message_rich_store_initialize(&r.store, {r.records, 2}, {r.order, 2}, {r.text, sizeof(r.text)});
    GatewayMessageEvent message{};
    const auto call = utf8_literal(R"({"op":0,"s":2,"t":"MESSAGE_CREATE","d":{
        "id":"200","channel_id":"8","author":{"id":"6","username":"Lin"},"content":"",
        "timestamp":"2026-09-05T10:00:00Z","type":3,
        "call":{"participants":["6","5"],"ended_timestamp":"2026-09-05T10:05:00Z"}}})");
    ok = ok && decode_create(call, &message, 5);
    check(ok && message.call.present && message.call.ended && message.call.participated &&
              message.call.ended_timestamp_ms - message.timestamp_ms == 300'000,
          "system_call_object_survives_transport");
    ok = ok && message_rich_store_apply(&r.store, 200, 1, message).ok();
    const auto* stored = message_rich_store_find(r.store, 200);
    check(ok && stored && stored->system.call_present && stored->system.call_participated &&
              stored->system.message_type == 3,
          "system_call_stored");
    const auto recipient = utf8_literal(R"({"op":0,"s":3,"t":"MESSAGE_CREATE","d":{
        "id":"201","channel_id":"8","author":{"id":"6","username":"Lin"},"content":"",
        "timestamp":"2026-09-05T10:06:00Z","type":1,"mentions":[{"id":"9","username":"Kai"}]}})");
    message = {};
    ok = decode_create(recipient, &message) && message_rich_store_apply(&r.store, 201, 1, message).ok();
    stored = message_rich_store_find(r.store, 201);
    check(ok && stored && stored->system.target_user_id == 9 &&
              utf8_equal(message_rich_store_string(r.store, stored->system.target_name), utf8_literal("Kai")),
          "system_recipient_target_stored");
    message_rich_store_reset(&r.store);
    const auto pin = utf8_literal(R"({"op":0,"s":4,"t":"MESSAGE_CREATE","d":{
        "id":"202","channel_id":"8","author":{"id":"6","username":"Lin"},"content":"",
        "timestamp":"2026-09-05T10:07:00Z","type":6,"message_reference":{"message_id":"150","channel_id":"8"}}})");
    message = {};
    ok = decode_create(pin, &message) && message_rich_store_apply(&r.store, 202, 1, message).ok();
    stored = message_rich_store_find(r.store, 202);
    check(ok && stored && stored->reference.message_id == 150 && !message_rich_has_header(*stored),
          "system_pin_reference_is_an_action_not_a_reply");
    message_rich_store_release(&r.store);
}

void test_interactions(Check check) noexcept {
    char data_bytes[512];
    ProtocolWriter data{{reinterpret_cast<u8*>(data_bytes), sizeof(data_bytes)}, 0};
    check(message_interaction_write_component_data(2, utf8_literal("ok"), {}, &data) &&
              utf8_equal({data_bytes, data.used}, utf8_literal(R"({"component_type":2,"custom_id":"ok"})")),
          "interaction_button_data");
    const Utf8View values[]{utf8_literal("r"), utf8_literal("b")};
    data.used = 0;
    check(message_interaction_write_component_data(3, utf8_literal("pick"), {values, 2}, &data) &&
              utf8_equal({data_bytes, data.used},
                    utf8_literal(R"({"component_type":3,"custom_id":"pick","type":3,"values":["r","b"]})")),
          "interaction_select_data");
    data.used = 0;
    check(!message_interaction_write_component_data(2, utf8_literal("ok"), {values, 1}, &data),
          "interaction_button_rejects_values");

    static MessageInteractionRequest request{};
    request = {};
    request.application_id = 9;
    request.guild_id = 3;
    request.channel_id = 7;
    request.message_id = 100;
    request.nonce = 555;
    request.message_flags = 32768;
    request.type = MessageInteractionType::message_component;
    const auto button = utf8_literal(R"({"component_type":2,"custom_id":"ok"})");
    copy_bytes(request.data, button.data, button.bytes);
    request.data_bytes = button.bytes;
    char body[1024];
    ProtocolWriter writer{{reinterpret_cast<u8*>(body), sizeof(body)}, 0};
    check(message_interaction_write_body(request, utf8_literal("sess"), &writer) &&
              utf8_equal({body, writer.used},
                    utf8_literal(R"({"type":3,"nonce":"555","guild_id":"3","channel_id":"7","message_flags":32768,)"
                                 R"("message_id":"100","application_id":"9","session_id":"sess",)"
                                 R"("data":{"component_type":2,"custom_id":"ok"}})")),
          "interaction_component_body_matches_official_client");
    request.message_id = 0;
    check(!message_interaction_request_valid(request), "interaction_component_requires_message");
    request.type = MessageInteractionType::modal_submit;
    request.guild_id = 0;
    writer.used = 0;
    check(message_interaction_write_body(request, utf8_literal("sess"), &writer) &&
              utf8_equal({body, writer.used},
                    utf8_literal(R"({"type":5,"nonce":"555","channel_id":"7","application_id":"9",)"
                                 R"("session_id":"sess","data":{"component_type":2,"custom_id":"ok"}})")),
          "interaction_modal_body_omits_message");

    u32 structurals[512];
    MessageInteractionEvent event{};
    check(message_interaction_event_decode(utf8_literal(R"({"id":"77","nonce":"555"})"), {structurals, 512}, &event) &&
              event.id == 77 && event.nonce == 555,
          "interaction_event_decode");
    check(message_interaction_event_decode(utf8_literal(R"({"id":"78","nonce":"opaque","reason_code":5})"),
                                           {structurals, 512}, &event) &&
              event.nonce == 0,
          "interaction_event_opaque_nonce");

    const auto modal_json = utf8_literal(R"({"id":"900","nonce":"555","channel_id":"7","custom_id":"form",
        "title":"Feedback","application":{"id":"9","name":"Helper"},"components":[
          {"type":10,"content":"Tell us more."},
          {"type":1,"components":[{"type":4,"custom_id":"name","style":1,"label":"Name","min_length":2,"max_length":20,"placeholder":"You"}]},
          {"type":18,"label":"Details","description":"Optional","component":{"type":4,"custom_id":"body","style":2,"required":false,"value":"Hi"}}]})");
    char pool[kMessageModalPoolBytes];
    static MessageModal modal{};
    const bool modal_ok = message_modal_decode(modal_json, {structurals, 512}, {pool, sizeof(pool)}, &modal);
    check(modal_ok && modal.interaction_id == 900 && modal.nonce == 555 && modal.application_id == 9 &&
              modal.field_count == 2 && modal.paragraph_count == 1 && !modal.unsupported &&
              utf8_equal(modal.title, utf8_literal("Feedback")) && utf8_equal(modal.application_name, utf8_literal("Helper")),
          "modal_decode_fields_and_paragraphs");
    if (!modal_ok)
        return;
    const auto& name = modal.fields[0];
    const auto& details = modal.fields[1];
    check(name.style == MessageModalInputStyle::short_text && name.required && name.min_length == 2 &&
              name.max_length == 20 && !name.labelled && details.labelled &&
              details.style == MessageModalInputStyle::paragraph && !details.required &&
              utf8_equal(details.label, utf8_literal("Details")) && utf8_equal(details.value, utf8_literal("Hi")),
          "modal_field_properties");
    Utf8View reason{};
    check(!message_modal_value_valid(name, utf8_literal("A"), &reason) && utf8_equal(reason, utf8_literal("Too short")) &&
              !message_modal_value_valid(name, {}, &reason) && message_modal_value_valid(details, {}, &reason) &&
              message_modal_value_valid(name, utf8_literal("Ana"), &reason),
          "modal_value_validation");
    const Utf8View answers[]{utf8_literal("Ana"), utf8_literal("All \"good\"")};
    char submit[1024];
    ProtocolWriter submit_writer{{reinterpret_cast<u8*>(submit), sizeof(submit)}, 0};
    check(message_modal_write_submit_data(modal, {answers, 2}, &submit_writer) &&
              utf8_equal({submit, submit_writer.used},
                    utf8_literal(R"({"id":"900","custom_id":"form","components":[)"
                                 R"({"type":1,"components":[{"type":4,"custom_id":"name","value":"Ana"}]},)"
                                 R"({"type":18,"component":{"type":4,"custom_id":"body","value":"All \"good\""}}]})")),
          "modal_submit_data_mirrors_structure");
    const Utf8View short_answer[]{utf8_literal("A"), utf8_literal("x")};
    submit_writer.used = 0;
    check(!message_modal_write_submit_data(modal, {short_answer, 2}, &submit_writer),
          "modal_submit_rejects_invalid_values");
    check(!message_modal_decode(utf8_literal(R"({"id":"1","custom_id":"x","components":[]})"), {structurals, 512},
                                {pool, sizeof(pool)}, &modal),
          "modal_requires_fields");

    ApiRejection automod{200000, true, false}, dm{50007, true, false}, other{50013, true, false};
    const auto notice = message_send_rejection_notice_input(7, message_send_rejection_system_notice(automod),
                                                            1'800'000'000'000ull, 1);
    check(test_contains(message_send_rejection_system_notice(automod), utf8_literal("AutoMod")) &&
              test_contains(message_send_rejection_system_notice(dm), utf8_literal("couldn't be delivered")) &&
              !message_send_rejection_system_notice(other).bytes && notice.type == kMessageTypeLocalNotice &&
              (notice.flags & kMessageFlagEphemeral) && notice.delivery == MessageDeliveryState::pending &&
              snowflake_timestamp_ms(notice.id) == 1'800'000'000'000ull,
          "send_rejections_become_local_notices");
}

void test_invite_cache(Check check) noexcept {
    static InviteCardCache cache{};
    cache = {};
    const auto code = utf8_literal("abc123");
    const auto* entry = invite_card_cache_touch(&cache, code, 1000);
    check(entry && entry->state == InviteCardState::queued && invite_card_cache_next(cache) == entry,
          "invite_cache_queues_new_codes");
    check(!invite_card_cache_touch(&cache, utf8_literal("bad code!"), 1000), "invite_cache_rejects_invalid_codes");
    GuildInvite invite{};
    invite.guild_id = 44;
    invite.member_count = 5678;
    invite.presence_count = 1234;
    invite.counts_known = true;
    check(invite_card_cache_begin(&cache, code) && entry->state == InviteCardState::pending &&
              !invite_card_cache_next(cache) &&
              invite_card_cache_complete(&cache, code, GuildInviteOutcome::resolved, &invite, 2000) &&
              entry->state == InviteCardState::resolved && entry->invite.guild_id == 44,
          "invite_cache_resolves");
    invite_card_cache_touch(&cache, code, 2000 + kInviteCardFreshMs);
    check(entry->state == InviteCardState::queued && entry->invite.guild_id == 44,
          "invite_cache_refreshes_stale_entries_keeping_data");
    invite_card_cache_begin(&cache, code);
    invite_card_cache_complete(&cache, code, GuildInviteOutcome::unavailable, nullptr, 3000);
    check(entry->state == InviteCardState::resolved, "invite_cache_keeps_data_on_failed_refresh");
    const auto gone = utf8_literal("gone");
    invite_card_cache_touch(&cache, gone, 3000);
    invite_card_cache_begin(&cache, gone);
    invite_card_cache_complete(&cache, gone, GuildInviteOutcome::unknown, nullptr, 3000);
    invite_card_cache_touch(&cache, gone, 999'999'999);
    check(invite_card_cache_find(cache, gone)->state == InviteCardState::invalid, "invite_cache_invalid_is_final");
    const auto flaky = utf8_literal("flaky");
    invite_card_cache_touch(&cache, flaky, 3000);
    invite_card_cache_begin(&cache, flaky);
    invite_card_cache_complete(&cache, flaky, GuildInviteOutcome::unavailable, nullptr, 3000);
    invite_card_cache_touch(&cache, flaky, 3000 + kInviteCardRetryMs);
    check(invite_card_cache_find(cache, flaky)->state == InviteCardState::queued, "invite_cache_retries_failures");
    char name[8];
    for (u32 i = 0; i < kInviteCardCacheEntries + 4; ++i) {
        name[0] = 'c';
        name[1] = static_cast<char>('a' + i / 26);
        name[2] = static_cast<char>('a' + i % 26);
        invite_card_cache_touch(&cache, {name, 3}, 10'000'000 + i);
    }
    check(invite_card_cache_find(cache, code) == nullptr, "invite_cache_evicts_least_recent");
    char url[192];
    invite.icon_bytes = 4;
    copy_bytes(invite.icon, "abcd", 4);
    check(utf8_equal({url, message_invite_icon_url(invite, {url, sizeof(url)})},
                utf8_literal("https://cdn.discordapp.com/icons/44/abcd.png?size=128")),
          "invite_icon_url");
}

// Presentation: system rows, ephemeral footers, "thinking" and invite cards
// through the full text coordinator.
constexpr u32 kRows = 4;
constexpr u32 kWidgets = (kMessageSceneWidgetsPerRow + 1) * kRows;
struct SceneFixture {
    MessageRecord record[kRows];
    u32 order[kRows];
    u8 text[2048];
    MessageRichRecord rich_record[kRows];
    u16 rich_order[kRows];
    u8 rich_text[8192];
    u32 heights[kRows];
    u64 fenwick[kRows];
    UiWidget widget[kWidgets];
    UiLayoutNode node[kWidgets];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageTextCommand commands[256];
    char utf8[8192];
    TextStyleSpan styles[512];
    MessageMarkdownSemantic semantics[64];
    MessageTextMeasurement measurements[kRows];
    MessageTextSpoilerReveal spoilers[2];
    u32 byte_styles[4096];
    MessageTextPresentation presentation;
    UiTextMeasureRecord cache_records[256];
    UiTextMeasureCache cache;
    InviteCardCache invites;
};
SceneFixture scene_fixture;

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    const float width = minimum(request.constraint.width, static_cast<float>(request.text.bytes) * 7);
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 7));
    const u32 lines = request.no_wrap ? 1 : (request.text.bytes + columns - 1) / columns;
    *output = {{width, static_cast<float>(lines) * 18}, 14, lines};
    return true;
}
bool finish(void*, const Theme&) noexcept {
    return true;
}
bool command_text(const SceneFixture& f, u64 message_id, Utf8View part,
                  MessageTextMeasurementKind kind = MessageTextMeasurementKind::none, bool any_kind = true) noexcept {
    for (u32 i = 0; i < f.presentation.command_count; ++i)
        if (f.commands[i].message_id == message_id && (any_kind || f.commands[i].measurement_kind == kind) &&
            test_contains(f.commands[i].text, part))
            return true;
    return false;
}

void test_presentation(Check check) noexcept {
    auto& f = scene_fixture;
    f = {};
    bool ok = message_store_initialize(&f.store, {f.record, kRows}, {f.order, kRows}, {f.text, sizeof(f.text)}) &&
              message_rich_store_initialize(&f.rich, {f.rich_record, kRows}, {f.rich_order, kRows},
                                            {f.rich_text, sizeof(f.rich_text)});
    MessageInput join{};
    join.id = 3ull << 22;
    join.channel_id = 7;
    join.author_id = 11;
    join.timestamp_ms = 1'500'000'000'000ull;
    join.type = message_type_user_join;
    MessageInput ephemeral{};
    ephemeral.id = (3ull << 22) + 1;
    ephemeral.channel_id = 7;
    ephemeral.author_id = 9;
    ephemeral.timestamp_ms = join.timestamp_ms + 1;
    ephemeral.content = utf8_literal("Saved!");
    ephemeral.flags = 64;
    MessageInput thinking = ephemeral;
    thinking.id += 1;
    thinking.content = {};
    thinking.flags = 128;
    MessageInput invite = ephemeral;
    invite.id += 2;
    invite.flags = 0;
    invite.content = utf8_literal("join us https://discord.gg/abc123 today");
    ok = ok && message_store_upsert(&f.store, join).ok() && message_store_upsert(&f.store, ephemeral).ok() &&
         message_store_upsert(&f.store, thinking).ok() && message_store_upsert(&f.store, invite).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, kRows}, {f.fenwick, kRows}, kRows, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(2000), false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, kRows, kRows, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 512}, {f.semantics, 64}, {f.measurements, kRows},
                                              {f.spoilers, 2}, {f.byte_styles, 4096}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 256}, measure, nullptr);
    message_scene_bind_invite_cards(&f.scene, &f.invites);
    ChatStateStore state{};
    NavigationView navigation{};
    const auto converge = [&]() noexcept {
        const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr,
                                                    &navigation, &kDefaultTheme, finish, nullptr, 1, 1};
        return message_scene_update(&f.scene, {0, 0, 700, 2000}).ok() &&
               message_text_coordinator_converge(request).ok();
    };
    ok = ok && converge();
    check(ok, "presentation_converges_with_wave_rows");
    if (!ok) {
        message_scene_release(&f.scene);
        return;
    }
    const auto* join_row = message_scene_find_row(f.scene, 0);
    check(join_row && join_row->grouped &&
              command_text(f, join.id, utf8_literal("Someone"), MessageTextMeasurementKind::content, false),
          "presentation_system_row_is_headerless_text");
    const auto* ephemeral_row = message_scene_find_row(f.scene, 1);
    check(ephemeral_row && ephemeral_row->rich_layout.notice > 0 &&
              command_text(f, ephemeral.id, utf8_literal("Only you can see this")),
          "presentation_ephemeral_footer");
    check(command_text(f, thinking.id, utf8_literal("is thinking")), "presentation_loading_thinks");
    const auto status_icon = [&](MessageMarkdownChipIcon icon) noexcept {
        for (u32 i = 0; i < f.presentation.command_count; ++i) {
            const auto& command = f.commands[i];
            if (command.message_id != thinking.id)
                continue;
            for (u32 s = 0; s < command.semantics.count; ++s)
                if (command.semantics[s].kind == MessageMarkdownSemanticKind::chip_icon &&
                    command.semantics[s].target == static_cast<u64>(icon) && command.semantics[s].byte_offset == 0)
                    return true;
        }
        return false;
    };
    check(status_icon(MessageMarkdownChipIcon::pending_dots) && f.presentation.status_dots,
          "presentation_loading_leads_with_the_status_dots");
    const u64 thinking_deadline = snowflake_timestamp_ms(thinking.id) / 1000 + 15 * 60;
    check(f.presentation.next_timestamp_update_unix_seconds &&
              f.presentation.next_timestamp_update_unix_seconds <= thinking_deadline,
          "presentation_loading_schedules_its_response_deadline");
    {
        const MessageTextCoordinatorRequest late{&f.scene, &f.presentation, &f.cache, &state, nullptr,
                                                 &navigation, &kDefaultTheme, finish, nullptr,
                                                 thinking_deadline, 1};
        check(message_text_coordinator_converge(late).ok() &&
                  command_text(f, thinking.id, utf8_literal("The application did not respond")) &&
                  !command_text(f, thinking.id, utf8_literal("is thinking")),
              "presentation_loading_times_out_after_fifteen_minutes");
        check(status_icon(MessageMarkdownChipIcon::failed) && !f.presentation.status_dots,
              "presentation_unanswered_loading_shows_the_failed_icon");
        check(converge(), "presentation_restores_the_fixture_clock");
    }
    const auto* invite_row = message_scene_find_row(f.scene, 3);
    check(invite_row && invite_row->invite_item_count == 1 && invite_row->rich_layout.invites == kMessageInviteCardHeight &&
              command_text(f, invite.id, utf8_literal("Loading invite")),
          "presentation_invite_card_loading");
    GuildInvite resolved{};
    resolved.guild_id = 44;
    resolved.name_bytes = 4;
    copy_bytes(resolved.name, "Cafe", 4);
    resolved.member_count = 5678;
    resolved.presence_count = 1234;
    resolved.counts_known = true;
    invite_card_cache_touch(&f.invites, utf8_literal("abc123"), 1);
    invite_card_cache_begin(&f.invites, utf8_literal("abc123"));
    invite_card_cache_complete(&f.invites, utf8_literal("abc123"), GuildInviteOutcome::resolved, &resolved, 2);
    f.scene.dirty = true;
    ok = converge();
    check(ok && command_text(f, invite.id, utf8_literal("Cafe")) &&
              command_text(f, invite.id, utf8_literal("1,234 online \xC2\xB7 5,678 members")) &&
              command_text(f, invite.id, utf8_literal("Join")),
          "presentation_invite_card_resolved");
    // A failed interaction annotates its message.
    message_scene_set_interaction(&f.scene, {ephemeral.id, 0, false, true});
    ok = converge();
    check(ok && command_text(f, ephemeral.id, utf8_literal("This interaction failed")),
          "presentation_failed_interaction_notice");
    PaintStream paint{};
    MessageMediaCommand media_storage[64];
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 800, 2000) &&
              message_media_command_stream_initialize(&media, {media_storage, 64}) &&
              message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &painted),
          "presentation_paints_wave_rows");
    paint_stream_release(&paint);
    message_scene_release(&f.scene);
}


// Plugin decorations (mc.msg.render): the scene, text projection and author
// header read cached verdicts through the UI query; nothing evaluates here.
u64 g_render_ids[kRows];
UiPluginRenderDecoration g_render_decorations[kRows];
bool render_query(u64 id, u32, UiPluginRenderDecoration* output) noexcept {
    for (u32 i = 0; i < kRows; ++i)
        if (g_render_ids[i] == id) {
            *output = g_render_decorations[i];
            return true;
        }
    return false;
}
const MessageTextCommand* find_command(const SceneFixture& f, u64 message_id, Utf8View part) noexcept {
    for (u32 i = 0; i < f.presentation.command_count; ++i)
        if (f.commands[i].message_id == message_id && test_contains(f.commands[i].text, part))
            return &f.commands[i];
    return nullptr;
}
bool has_target(const MessageTextCommand* command, u64 target) noexcept {
    for (u32 i = 0; command && i < command->semantics.count; ++i)
        if (command->semantics[i].target == target && command->semantics[i].kind == MessageMarkdownSemanticKind::link &&
            ui_plugin_render_source(static_cast<u16>(command->semantics[i].source_offset >> 16u)))
            return true;
    return false;
}
struct RenderUsers {
    UserRecord records[4];
    u32 order[4];
    u8 names[256];
    UserStore store;
};
RenderUsers render_users;

void test_plugin_decorations(Check check) noexcept {
    auto& f = scene_fixture;
    f = {};
    render_users = {};
    bool ok = message_store_initialize(&f.store, {f.record, kRows}, {f.order, kRows}, {f.text, sizeof(f.text)}) &&
              message_rich_store_initialize(&f.rich, {f.rich_record, kRows}, {f.rich_order, kRows},
                                            {f.rich_text, sizeof(f.rich_text)}) &&
              user_store_initialize(&render_users.store, {render_users.records, 4}, {render_users.order, 4},
                                    {render_users.names, sizeof(render_users.names)}) &&
              user_store_upsert(&render_users.store, 11, utf8_literal("Ann")).ok() &&
              user_store_upsert(&render_users.store, 9, utf8_literal("Bob")).ok();
    MessageInput first{};
    first.id = 5ull << 22;
    first.channel_id = 7;
    first.author_id = 11;
    first.timestamp_ms = 1'500'000'000'000ull;
    first.content = utf8_literal("first message");
    MessageInput hidden = first;
    hidden.id += 1;
    hidden.author_id = 9;
    hidden.timestamp_ms += 1;
    hidden.content = utf8_literal("hidden secret");
    MessageInput replaced = hidden;
    replaced.id += 1;
    replaced.timestamp_ms += 1;
    replaced.content = utf8_literal("stored text");
    MessageInput collapsed = replaced;
    collapsed.id += 1;
    collapsed.timestamp_ms += 1;
    collapsed.content = utf8_literal("collapsed secret");
    ok = ok && message_store_upsert(&f.store, first).ok() && message_store_upsert(&f.store, hidden).ok() &&
         message_store_upsert(&f.store, replaced).ok() && message_store_upsert(&f.store, collapsed).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, kRows}, {f.fenwick, kRows}, kRows, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(2000), false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, kRows, kRows, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 512}, {f.semantics, 64}, {f.measurements, kRows},
                                              {f.spoilers, 2}, {f.byte_styles, 4096}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 256}, measure, nullptr);
    for (auto& decoration : g_render_decorations)
        decoration = {};
    g_render_ids[0] = first.id;
    g_render_ids[1] = hidden.id;
    g_render_ids[2] = replaced.id;
    g_render_ids[3] = collapsed.id;
    auto& decorated = g_render_decorations[0];
    decorated.name_suffix = utf8_literal("(away)");
    decorated.name_tone = kUiPluginToneRole;
    decorated.media = ui_plugin_media_blur;
    decorated.badge_count = 1;
    decorated.badges[0] = {0x55, 0, utf8_literal("VIP"), {}, {}, kUiPluginRenderNoIcon, 2, 1};
    decorated.accessory_count = 3;
    decorated.accessories[0] = {0x55, 0, utf8_literal("Note"), {}, {}, kUiPluginRenderNoIcon, kUiPluginRenderLabel, 0};
    decorated.accessories[1] = {0x55, 7, utf8_literal("Undo"), {}, {}, kUiPluginRenderNoIcon, kUiPluginRenderButton, 1};
    decorated.accessories[2] = {0x55, 0, utf8_literal("Docs"), {}, utf8_literal("https://example.com/docs"),
                                kUiPluginRenderNoIcon, kUiPluginRenderLink, 0};
    g_render_decorations[1].visibility = ui_plugin_render_hidden;
    g_render_decorations[2].replace_content = true;
    g_render_decorations[2].content = utf8_literal("**replaced** text");
    g_render_decorations[2].name_prefix = utf8_literal("[bot]");
    g_render_decorations[3].visibility = ui_plugin_render_collapsed;
    g_ui_plugin_render_query = render_query;
    g_ui_plugin_render_revision = 1;
    f.scene.plugin_decorations = true;
    ChatStateStore state{};
    NavigationView navigation{};
    const auto converge = [&]() noexcept {
        const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, &render_users.store,
                                                    &navigation, &kDefaultTheme, finish, nullptr, 1, 1};
        return message_scene_update(&f.scene, {0, 0, 700, 2000}).ok() &&
               message_text_coordinator_converge(request).ok();
    };
    ok = ok && converge();
    check(ok, "plugin_render_converges");
    if (!ok) {
        g_ui_plugin_render_query = nullptr;
        message_scene_release(&f.scene);
        return;
    }
    const auto* hidden_row = message_scene_find_row(f.scene, 1);
    check(hidden_row && hidden_row->redacted && ui_variable_list_item_units(f.list, 1) == 1 &&
              !find_command(f, hidden.id, utf8_literal("secret")),
          "plugin_render_hidden_row_collapses_to_one_unit");
    const auto* replaced_row = message_scene_find_row(f.scene, 2);
    const auto* collapsed_row = message_scene_find_row(f.scene, 3);
    check(replaced_row && !replaced_row->grouped && collapsed_row && collapsed_row->grouped,
          "plugin_render_hidden_row_never_starts_a_group");
    const auto* first_row = message_scene_find_row(f.scene, 0);
    const auto* undo = find_command(f, first.id, utf8_literal("Undo"));
    check(first_row && first_row->rich_layout.notice >= 20 && find_command(f, first.id, utf8_literal("Note")) && undo &&
              undo->measurement_kind == MessageTextMeasurementKind::notice && has_target(undo, 2) &&
              has_target(find_command(f, first.id, utf8_literal("Docs")), 3),
          "plugin_render_accessory_row_maps_actions");
    const auto* body = find_command(f, replaced.id, utf8_literal("replaced"));
    const Utf8View body_source = body ? message_text_presentation_command_source(f.store, &f.rich, *body) : Utf8View{};
    check(body && body->source_id == kUiPluginRenderContentSource && !find_command(f, replaced.id, utf8_literal("stored")) &&
              utf8_equal(body_source, utf8_literal("**replaced** text")),
          "plugin_render_replacement_has_its_own_source");
    const auto* folded = find_command(f, collapsed.id, utf8_literal("Message hidden by a plugin"));
    check(folded && test_contains(folded->text, utf8_literal("Show")) && has_target(folded, kUiPluginRenderShowTarget) &&
              !find_command(f, collapsed.id, utf8_literal("collapsed secret")),
          "plugin_render_collapsed_label_and_show_link");
    // Cozy header: the suffix follows the name and the badge follows it.
    const MessageTextCommand* author = nullptr;
    for (u32 i = 0; i < f.presentation.command_count; ++i)
        if (first_row && f.commands[i].owner.handle.slot == first_row->author.slot &&
            f.commands[i].owner.handle.generation == first_row->author.generation)
            author = &f.commands[i];
    MessageAuthorLayout header{};
    const bool laid = author && message_author_layout(f.scene, *author, state, render_users.store, &f.cache,
                                                      kDefaultTheme, &header);
    check(laid && utf8_equal(header.name.text, utf8_literal("Ann")) && utf8_equal(header.name_suffix.text, utf8_literal("(away)")) &&
              header.name_suffix.rectangle.left > header.name.rectangle.right && header.decoration_count == 1 &&
              header.decorations[0].badge.rectangle.left > header.name_suffix.rectangle.right,
          "plugin_render_cozy_suffix_and_badge");
    const auto profile = laid ? message_author_hit_test(f.scene, f.presentation, state, render_users.store, &f.cache,
                                                        kDefaultTheme, header.name.rectangle.left + 4,
                                                        (header.name.rectangle.top + header.name.rectangle.bottom) * .5f)
                              : MessageProfileHit{};
    check(profile.user_id == 11, "plugin_render_cozy_name_hit");
    // Media verdicts through the scene funnel; zero ignores the plugin.
    const u64 key = message_media_reveal_key(first.id, MessageMediaSource::attachment, 0);
    MediaPolicyFixture media_policy{{MessageExplicitFilter::blur, false}, {}, 0};
    message_scene_set_media_policy(&f.scene, {&media_policy, query_media_policy, 1});
    const MessageMediaOwner media_owner{first.channel_id, first.author_id, 0, 0, false};
    constexpr MessageMediaPolicyResult fallback{MessageExplicitFilter::show, true};
    check(message_scene_media_obscure(f.scene, fallback, first.id, key, false, false) ==
                  MessageMediaObscure::explicit_content &&
              message_scene_media_obscure(f.scene, fallback, 0, key, false, false) == MessageMediaObscure::none,
          "plugin_render_media_blur");
    decorated.media = ui_plugin_media_hide;
    check(message_scene_media_obscure(f.scene, fallback, first.id, key, false, false) ==
                  MessageMediaObscure::plugin_hidden &&
              utf8_equal(message_media_obscure_label(MessageMediaObscure::plugin_hidden),
                    utf8_literal("Media hidden by a plugin")),
          "plugin_render_media_hide");
    decorated.media = ui_plugin_media_blur;
    check(message_scene_reveal_media(&f.scene, key) &&
              message_scene_media_obscure(f.scene, media_owner, first.id, key, false, false) == MessageMediaObscure::none,
          "plugin_render_blur_reveals_on_click");
    message_scene_set_media_policy(&f.scene, {});
    PaintStream paint{};
    MessageMediaCommand media_storage[64];
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 800, 2000) &&
              message_media_command_stream_initialize(&media, {media_storage, 64}) &&
              message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &painted),
          "plugin_render_paints_decorated_rows");
    paint_stream_release(&paint);
    // Compact rows: "[bot] Bob" keeps the name hit after the prefix.
    message_scene_set_display(&f.scene, {0, true});
    ok = converge();
    const auto* compact = find_command(f, replaced.id, utf8_literal("[bot] Bob"));
    const float name_x = compact ? compact->rectangle.left + 6 * 7 + 4 : 0;
    const float line_y = compact ? compact->rectangle.top + 6 : 0;
    check(ok && compact && compact->text.data[0] == '[' &&
              message_author_compact_hit(f.scene, *compact, &f.cache, name_x, line_y).user_id == 9 &&
              !message_author_compact_hit(f.scene, *compact, &f.cache, compact->rectangle.left + 4, line_y).user_id,
          "plugin_render_compact_prefix_keeps_name_hit");
    // A new verdict revision that shows the hidden row restores it.
    g_render_decorations[1].visibility = ui_plugin_render_normal;
    ++g_ui_plugin_render_revision;
    message_scene_set_display(&f.scene, {0, false});
    ok = converge();
    hidden_row = message_scene_find_row(f.scene, 1);
    check(ok && hidden_row && !hidden_row->redacted && ui_variable_list_item_units(f.list, 1) > 1 &&
              find_command(f, hidden.id, utf8_literal("hidden secret")),
          "plugin_render_unhidden_row_returns");
    g_ui_plugin_render_query = nullptr;
    g_ui_plugin_render_revision = 0;
    message_scene_release(&f.scene);
}
} // namespace

void test_message_content_wave(Check check) noexcept {
    test_system_text(check);
    test_media_policy(check);
    test_components(check);
    test_system_records(check);
    test_interactions(check);
    test_invite_cache(check);
    test_presentation(check);
    test_plugin_decorations(check);
}

} // namespace maicord

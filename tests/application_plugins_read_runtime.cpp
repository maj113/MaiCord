// mc.read sources and the Gateway event translation over synthetic stores.
#include "application_state.hpp"
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/protocol/client_status.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace plugins_read_runtime {
using namespace app_detail;

RelationshipStore relationships;
RelationshipRecord relationship_rows[4];
bool relationships_ready;

struct Storage {
    RoleRecord roles[8];
    PermissionOverwriteRecord overwrites[4];
    MessageRecord message_records[8];
    u32 message_order[8];
    u8 message_text[1024];
    u16 rich_order[4];
    u8 rich_text[2048];
    StableStorage rich_records;
};
Storage storage;

struct Collected {
    u32 count;
    NativePluginSdkEvent events[8];
    char text[8][64];
    char name[8][64];
};
bool collect(void* context, const NativePluginSdkEvent& event) noexcept {
    auto& out = *static_cast<Collected*>(context);
    if (out.count >= 8) return false;
    auto& copy = out.events[out.count];
    copy = event;
    copy.text = {out.text[out.count], utf8_copy_truncated(out.text[out.count], event.text)};
    copy.name = {out.name[out.count], utf8_copy_truncated(out.name[out.count], event.name)};
    ++out.count;
    return true;
}
bool equals(Utf8View view, const char* literal) noexcept {
    u32 bytes = 0;
    while (literal[bytes]) ++bytes;
    return view.bytes == bytes && bytes_equal(view.data, literal, bytes);
}
template <u32 N> bool equals(const char (&text)[N], u32 bytes, const char* literal) noexcept {
    return equals(Utf8View{text, bytes}, literal);
}
GatewayClientEvent dispatch(GatewayClientDispatchKind kind) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    return event;
}

constexpr u64 kSelf = 100, kOther = 200, kGuild = 1000, kChannel = 1100, kRole = 1200;

const char* populate_failure;
bool need(bool value, const char* step) noexcept {
    if (!value && !populate_failure) populate_failure = step;
    return value;
}
bool populate(AccountUiRuntime& ui) noexcept {
    populate_failure = nullptr;
    bool ok = need(chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 4}, {ui.guild_order_storage, 4},
                                          {ui.channel_record_storage, 8}, {ui.channel_order_storage, 8},
                                          {ui.chat_state_name_storage, 4096}) &&
              chat_state_store_bind_permissions(&ui.chat_state, {storage.roles, 8}, {storage.overwrites, 4}),
              "plugin read setup: chat state");
    ui.chat_state.self_user_id = kSelf;
    GuildInput guild{kGuild, utf8_literal("Guild"), GuildMutationKind::create, kSelf};
    guild.upload_features = guild_record_community;
    guild.upload_features_known = true;
    ok = ok && need(chat_state_store_upsert_guild(&ui.chat_state, guild).ok(), "plugin read setup: guild");
    ChannelInput channel{kChannel, kGuild, 0, utf8_literal("general"), 3, 0, 0};
    channel.name_known = true;
    channel.last_message_id = 5002;
    channel.last_message_known = true;
    channel.content_policy = kChannelContentAgeRestricted;
    channel.content_policy_known = true;
    ok = ok && need(chat_state_store_upsert_channel(&ui.chat_state, channel).ok(), "plugin read setup: channel");
    RoleInput role{kRole, kGuild, 8, utf8_literal("Mods")};
    role.color = 0x123456;
    role.hoist = true;
    ok = ok && need(chat_state_store_upsert_role(&ui.chat_state, role), "plugin read setup: role");

    ok = ok && need(user_store_initialize(&ui.user_store, {ui.user_record_storage, 16}, {ui.user_order_storage, 16},
                                     {ui.user_name_storage, 4096}) &&
         user_store_upsert(&ui.user_store, kSelf, utf8_literal("Me"), utf8_literal("me")).ok() &&
         user_store_upsert(&ui.user_store, kOther, utf8_literal("Other"), utf8_literal("other")).ok() &&
         user_store_set_avatar(&ui.user_store, kOther, utf8_literal("0123456789abcdef0123456789abcdef")),
                    "plugin read setup: users");
    user_store_set_client_status(&ui.user_store, kOther,
                                 client_status_with(0, ClientPlatform::mobile, ClientPlatformStatus::idle));
    UserAppearance bot{};
    bot.user_id = kOther;
    bot.fields = appearance_bot;
    bot.bot = true;
    ok = ok && need(user_appearance_upsert(&ui.user_store.appearance, bot), "plugin read setup: appearance");
    UserActivitySet activities{};
    activities.user_id = kOther;
    activities.count = 1;
    activities.items[0].type = UserActivityType::listening;
    activities.items[0].start_ms = 77;
    copy_bytes(activities.items[0].text, "MusicSong", 9); // name then details, packed in field order
    activities.items[0].lengths[static_cast<u32>(UserActivityField::name)] = 5;
    activities.items[0].lengths[static_cast<u32>(UserActivityField::details)] = 4;
    ok = ok && need(user_activity_upsert(&ui.user_store.activities, activities),
                    "plugin read setup: activities");

    const u64 member_roles[]{kRole};
    ok = ok && need(guild_member_store_initialize(&ui.guild_member_store, {ui.guild_member_record_storage, 8},
                                             {ui.guild_member_order_storage, 8}) &&
         guild_member_store_upsert(&ui.guild_member_store, ui.chat_state, kGuild, kOther, utf8_literal("Nick"),
                                   {member_roles, 1}, true, GuildMemberStatus::dnd).ok(),
                    "plugin read setup: member") &&
         need(guild_member_store_observe_list(&ui.guild_member_store, kGuild, 77, 42, 7, true, true, false, 0, 42),
              "plugin read setup: member list");

    static VoiceState voice_records[8];
    ok = ok && need(voice_state_store_initialize_sparse(&ui.voice_states, {voice_records, 8}) &&
         voice_state_store_apply(&ui.voice_states, kGuild, kChannel, kOther, voice_self_mute | voice_deaf) ==
             VoiceStateResult::changed,
                    "plugin read setup: voice");

    ok = ok && relationship_store_initialize(&relationships, {relationship_rows, 4});
    RelationshipRecord row{};
    row.user_id = kOther;
    row.type = RelationshipType::friend_;
    row.presence = RelationshipPresence::online;
    row.nickname_bytes = 3;
    copy_bytes(row.nickname, "Pal", 3);
    ok = ok && need(relationship_store_upsert(&relationships, row).ok(), "plugin read setup: friends");
    relationships_ready = ok;

    auto& messages = g_app.message_view.store;
    ok = ok && need(message_store_initialize(&messages, {storage.message_records, 8}, {storage.message_order, 8},
                                        {storage.message_text, sizeof(storage.message_text)}),
                    "plugin read setup: message store");
    MessageInput first{5001, kChannel, kOther, 10, 0, utf8_literal("hello")};
    MessageInput second{5002, kChannel, kSelf, 20, 25, utf8_literal("mine")};
    second.presentation_flags = message_presentation_pinned;
    ok = ok && need(message_store_upsert(&messages, first).ok() && message_store_upsert(&messages, second).ok(),
                    "plugin read setup: messages");

    ok = ok && need(message_rich_store_initialize_owned_sparse(&ui.message_rich_store, &storage.rich_records, 4,
                                                          {storage.rich_order, 4},
                                                          {storage.rich_text, sizeof(storage.rich_text)}),
                    "plugin read setup: rich store");
    GatewayMessageReaction reaction{0, utf8_literal("👍"), 3, 3, 0, true, false, false};
    GatewayMessageAttachment attachment{};
    attachment.id = 901;
    attachment.filename = utf8_literal("SPOILER_cat.png");
    attachment.url = utf8_literal("https://cdn.example/cat.png");
    attachment.size = 64;
    attachment.width = 10;
    attachment.height = 20;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEvent rich{};
    rich.id = 5001;
    rich.channel_id = kChannel;
    rich.fields = gateway_message_field_attachments | gateway_message_field_reactions;
    rich.attachments = {&attachment, 1};
    rich.reactions = {&reaction, 1};
    const auto found = message_store_find(messages, 5001);
    const auto* record = found.ok() ? message_store_resolve(messages, found.handle) : nullptr;
    ok = ok && need(record && message_rich_store_apply(&ui.message_rich_store, 5001, record->revision, rich).ok(),
                    "plugin read setup: rich message");
    return ok;
}

void test_reads(const NativePluginSdkReadSource& read, void (*check)(bool, const char*) noexcept) noexcept {
    mc_guild_info guild{};
    check(read.guild(read.context, kGuild, &guild) && guild.guild_id == kGuild && guild.owner_id == kSelf &&
              (guild.flags & (MC_GUILD_OWNER | MC_GUILD_COMMUNITY | MC_GUILD_COUNTS)) ==
                  (MC_GUILD_OWNER | MC_GUILD_COMMUNITY | MC_GUILD_COUNTS) &&
              guild.member_count == 42 && guild.online_count == 7 && equals(guild.name, guild.name_bytes, "Guild") &&
              !read.guild(read.context, 9, &guild),
          "plugin read: guild copies owner, community, counts and name; unknown guild is not found");
    mc_channel_info channel{};
    check(read.channel(read.context, kChannel, &channel) && channel.guild_id == kGuild && channel.position == 3 &&
              channel.last_message_id == 5002 && (channel.flags & MC_CHANNEL_NSFW) &&
              !(channel.flags & (MC_CHANNEL_PRIVATE | MC_CHANNEL_THREAD)) &&
              equals(channel.name, channel.name_bytes, "general"),
          "plugin read: channel copies placement, age restriction and name");
    mc_role_info role{};
    check(read.role(read.context, kGuild, kRole, &role) && role.permissions == 8 && role.colour == 0x123456 &&
              role.flags == MC_ROLE_HOIST && equals(role.name, role.name_bytes, "Mods"),
          "plugin read: role copies permissions, colour data and flags");
    mc_member_info member{};
    check(read.member(read.context, kGuild, kOther, &member) && member.status == MC_STATUS_DND &&
              (member.flags & MC_MEMBER_ROLES) && member.role_count == 1 && member.role_ids[0] == kRole &&
              equals(member.nick, member.nick_bytes, "Nick") && !read.member(read.context, kGuild, kSelf, &member),
          "plugin read: member copies status, roles and nickname; non-resident members are not found");
    mc_user_info user{}, self{};
    check(read.user(read.context, kOther, &user) && !(user.flags & MC_USER_SELF) &&
              equals(user.name, user.name_bytes, "Other") && equals(user.username, user.username_bytes, "other") &&
              equals(user.avatar_hash, user.avatar_bytes, "0123456789abcdef0123456789abcdef") && read.user(read.context, kSelf, &self) &&
              (self.flags & MC_USER_SELF),
          "plugin read: users copy names and avatar and mark your account");
    mc_relationship_info relationship{};
    check(read.relationship(read.context, kOther, &relationship) && relationship.type == MC_RELATIONSHIP_FRIEND &&
              relationship.status == MC_STATUS_ONLINE &&
              equals(relationship.nickname, relationship.nickname_bytes, "Pal") &&
              !read.relationship(read.context, kSelf, &relationship),
          "plugin read: friends carry type, presence and nickname");
    mc_presence_info in_guild{}, global{};
    check(read.presence(read.context, kOther, kGuild, &in_guild) && in_guild.status == MC_STATUS_DND &&
              read.presence(read.context, kOther, 0, &global) && global.status == MC_STATUS_ONLINE &&
              global.mobile == MC_STATUS_IDLE && global.desktop == MC_STATUS_OFFLINE && global.activity_count == 1,
          "plugin read: presence prefers the server list with a guild and friends without, with platforms");
    mc_activity_info activity{};
    check(read.activity(read.context, kOther, 0, &activity) && activity.type == MC_ACTIVITY_LISTENING &&
              activity.start_ms == 77 && equals(activity.name, activity.name_bytes, "Music") &&
              equals(activity.details, activity.details_bytes, "Song") &&
              !read.activity(read.context, kOther, 1, &activity),
          "plugin read: activities copy type, times and text");
    mc_voice_info voice{}, anywhere{};
    check(read.voice(read.context, kGuild, kOther, &voice) && voice.channel_id == kChannel &&
              voice.flags == (MC_VOICE_DEAF | MC_VOICE_SELF_MUTE) && read.voice(read.context, 0, kOther, &anywhere) &&
              anywhere.guild_id == kGuild && !read.voice(read.context, kGuild, kSelf, &voice),
          "plugin read: voice maps Gateway flags and searches every server with guild zero");
    mc_message_info message{};
    Utf8View content{};
    check(read.message(read.context, kChannel, 5001, &message, &content) && equals(content, "hello") &&
              message.author_id == kOther && message.guild_id == kGuild && (message.flags & MC_MESSAGE_BOT) &&
              (message.flags & MC_MESSAGE_MEDIA) && !(message.flags & MC_MESSAGE_OWN) &&
              message.attachment_count == 1 && message.reaction_count == 1 &&
              equals(message.author_name, message.author_name_bytes, "Nick"),
          "plugin read: cached message copies content, author nickname, bot and media flags");
    check(read.message(read.context, kChannel, 5002, &message, &content) &&
              (message.flags & (MC_MESSAGE_OWN | MC_MESSAGE_EDITED | MC_MESSAGE_PINNED)) ==
                  (MC_MESSAGE_OWN | MC_MESSAGE_EDITED | MC_MESSAGE_PINNED) &&
              !read.message(read.context, 9, 5002, &message, &content),
          "plugin read: own, edited and pinned flags; another channel is not found");
    mc_attachment_info attachment{};
    mc_reaction_info reaction{};
    mc_embed_info embed{};
    check(read.attachment(read.context, kChannel, 5001, 0, &attachment) && attachment.attachment_id == 901 &&
              attachment.flags == MC_ATTACHMENT_SPOILER && attachment.width == 10 && attachment.bytes == 64 &&
              equals(attachment.url, attachment.url_bytes, "https://cdn.example/cat.png") &&
              read.reaction(read.context, kChannel, 5001, 0, &reaction) && reaction.count == 3 &&
              reaction.flags == MC_REACTION_ME && equals(reaction.name, reaction.name_bytes, "👍") &&
              !read.embed(read.context, kChannel, 5001, 0, &embed) &&
              !read.attachment(read.context, kChannel, 5001, 1, &attachment),
          "plugin read: attachments, reactions and embeds come from the rich store by index");
    u64 ids[4]{};
    u32 count = 0, next = 0;
    check(read.list(read.context, MC_LIST_MESSAGES, kChannel, 0, {ids, 1}, &count, &next) && count == 1 &&
              ids[0] == 5002 && next == 1 &&
              read.list(read.context, MC_LIST_MESSAGES, kChannel, next, {ids, 4}, &count, &next) && count == 1 &&
              ids[0] == 5001 && next == 0,
          "plugin read: message list pages newest first with a cursor");
    check(read.list(read.context, MC_LIST_GUILDS, 0, 0, {ids, 4}, &count, &next) && count == 1 && ids[0] == kGuild &&
              read.list(read.context, MC_LIST_CHANNELS, kGuild, 0, {ids, 4}, &count, &next) && count == 1 &&
              ids[0] == kChannel && read.list(read.context, MC_LIST_ROLES, kGuild, 0, {ids, 4}, &count, &next) &&
              count == 1 && ids[0] == kRole &&
              read.list(read.context, MC_LIST_MEMBERS, kGuild, 0, {ids, 4}, &count, &next) && count == 1 &&
              ids[0] == kOther && read.list(read.context, MC_LIST_VOICE, kChannel, 0, {ids, 4}, &count, &next) &&
              count == 1 && ids[0] == kOther &&
              read.list(read.context, MC_LIST_RELATIONSHIPS, 0, 0, {ids, 4}, &count, &next) && count == 1 &&
              !read.list(read.context, MC_LIST_ROLES, 9, 0, {ids, 4}, &count, &next),
          "plugin read: lists cover guilds, channels, roles, members, voice and friends; unknown scopes fail");
    mc_read_revisions before{}, same{}, after{};
    read.revisions(read.context, &before);
    read.revisions(read.context, &same);
    voice_state_store_apply(&g_app.account_ui->voice_states, kGuild, 0, kOther, 0);
    read.revisions(read.context, &after);
    check(before.voice && same.voice == before.voice && after.voice == before.voice + 1 &&
              after.messages == before.messages && after.guilds == before.guilds,
          "plugin read: revisions move only for the class that changed");
    voice_state_store_apply(&g_app.account_ui->voice_states, kGuild, kChannel, kOther, voice_self_mute | voice_deaf);
    mc_profile_info profile{};
    check(!read.profile(read.context, kOther, &profile), "plugin read: profiles are readable only while open");
}

void test_events(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u64 all = MC_EVENT_MASK(MC_EVENT_KIND_COUNT) - 1;
    // MESSAGE_CREATE: content then author name in the payload.
    u8 payload[64];
    copy_bytes(payload, "hi there", 8);
    copy_bytes(payload + 8, "Bot", 3);
    auto create = dispatch(GatewayClientDispatchKind::message_create);
    create.message.id = 6001;
    create.message.channel_id = kChannel;
    create.message.author_id = kOther;
    create.message.content_bytes = 8;
    create.message.author_name_bytes = 3;
    create.message.transient_bytes = 11;
    create.message.fields = gateway_message_field_author_id | gateway_message_field_content;
    create.message.mention_flags = gateway_client_message_author_bot;
    create.dispatch_payload_bytes = 11;
    Collected out{};
    const u32 created = application_plugins_gateway_translate(create, {payload, 11}, all, &out, collect);
    check(created == 1 && out.events[0].kind == MC_EVENT_MESSAGE_CREATE && out.events[0].guild_id == kGuild &&
              out.events[0].user_id == kOther && out.events[0].flags == (MC_EVENT_BOT | MC_EVENT_TEXT) &&
              equals(out.events[0].text, "hi there") && equals(out.events[0].name, "Bot"),
          "plugin events: message create carries content, author, guild and bot flag");
    out = {};
    check(!application_plugins_gateway_translate(create, {payload, 11}, MC_EVENT_MASK(MC_EVENT_TYPING), &out,
                                                 collect) && !out.count,
          "plugin events: kinds outside the interest are never decoded or emitted");
    // MESSAGE_DELETE reads the cached copy before the store drops it.
    auto removed = dispatch(GatewayClientDispatchKind::message_delete);
    removed.message.id = 5002;
    removed.message.channel_id = kChannel;
    out = {};
    check(application_plugins_gateway_translate(removed, {}, all, &out, collect) == 1 &&
              out.events[0].kind == MC_EVENT_MESSAGE_DELETE && out.events[0].user_id == kSelf &&
              (out.events[0].flags & (MC_EVENT_CACHED | MC_EVENT_OWN)) == (MC_EVENT_CACHED | MC_EVENT_OWN) &&
              equals(out.events[0].text, "mine") && equals(out.events[0].name, "Me"),
          "plugin events: delete carries the cached text, author and own flag");
    u8 bulk_ids[16]{};
    byte_store_le64(bulk_ids, 5001);
    byte_store_le64(bulk_ids + 8, 7777);
    auto bulk = dispatch(GatewayClientDispatchKind::message_bulk_delete);
    bulk.message.id_count = 2;
    bulk.message.channel_id = kChannel;
    bulk.dispatch_payload_bytes = 16;
    out = {};
    check(application_plugins_gateway_translate(bulk, {bulk_ids, 16}, all, &out, collect) == 2 &&
              out.events[0].message_id == 5001 && (out.events[0].flags & (MC_EVENT_BULK | MC_EVENT_CACHED)) ==
                                                      (MC_EVENT_BULK | MC_EVENT_CACHED) &&
              out.events[1].message_id == 7777 && out.events[1].flags == MC_EVENT_BULK && !out.events[1].user_id,
          "plugin events: bulk delete yields one event per id, cached where held");
    auto reaction = dispatch(GatewayClientDispatchKind::message_reaction_add);
    reaction.reaction = {5001, kChannel, kSelf, 0, 4, 0, 0};
    reaction.dispatch_payload_bytes = 4;
    const char thumbs[] = "👍";
    out = {};
    check(application_plugins_gateway_translate(reaction, {reinterpret_cast<const u8*>(thumbs), 4}, all, &out,
                                                collect) == 1 &&
              out.events[0].kind == MC_EVENT_REACTION_ADD && out.events[0].flags == MC_EVENT_OWN &&
              equals(out.events[0].text, "👍") && out.events[0].guild_id == kGuild,
          "plugin events: reactions carry the emoji and reactor");
    auto typing = dispatch(GatewayClientDispatchKind::typing_start);
    typing.typing = {kChannel, 0, kOther, 1};
    auto presence = dispatch(GatewayClientDispatchKind::presence_update);
    presence.presence = {kGuild, kOther, GatewayClientMemberListStatus::idle, 0};
    out = {};
    check(application_plugins_gateway_translate(typing, {}, all, &out, collect) == 1 &&
              out.events[0].kind == MC_EVENT_TYPING && out.events[0].guild_id == kGuild &&
              application_plugins_gateway_translate(presence, {}, all, &out, collect) == 1 &&
              out.events[1].kind == MC_EVENT_PRESENCE && out.events[1].value == MC_STATUS_IDLE,
          "plugin events: typing and presence map ids and status");
    GatewayClientVoiceStateRecord rows[2]{{kGuild, kChannel, kOther, voice_self_mute | voice_deaf, 0},
                                          {kGuild, 0, kSelf, 0, 0}};
    rows[1].channel_id = kChannel + 1;
    rows[1].flags = voice_self_video;
    auto voice = dispatch(GatewayClientDispatchKind::voice_state_snapshot);
    voice.voice_states = {kGuild, 2, false};
    voice.dispatch_payload_bytes = sizeof(rows);
    out = {};
    check(application_plugins_gateway_translate(voice, {reinterpret_cast<const u8*>(rows), sizeof(rows)}, all, &out,
                                                collect) == 1 &&
              out.events[0].user_id == kSelf && out.events[0].channel_id == kChannel + 1 &&
              out.events[0].value == MC_VOICE_VIDEO && (out.events[0].flags & MC_EVENT_OWN),
          "plugin events: voice emits only changed states with MC_VOICE flags");
    voice.voice_states.replace = true;
    out = {};
    check(!application_plugins_gateway_translate(voice, {reinterpret_cast<const u8*>(rows), sizeof(rows)}, all, &out,
                                                 collect),
          "plugin events: a replacing guild voice snapshot is discovery, not a change");
    auto channel = dispatch(GatewayClientDispatchKind::channel_create);
    channel.state.id = 1101;
    channel.state.guild_id = kGuild;
    channel.state.type = 2;
    channel.state.name_bytes = 5;
    channel.state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id |
                           gateway_client_state_field_type | gateway_client_state_field_name;
    channel.dispatch_payload_bytes = 5;
    auto deleted = dispatch(GatewayClientDispatchKind::channel_delete);
    deleted.state.id = kChannel;
    deleted.state.fields = gateway_client_state_field_id;
    out = {};
    check(application_plugins_gateway_translate(channel, {reinterpret_cast<const u8*>("voice"), 5}, all, &out,
                                                collect) == 1 &&
              out.events[0].kind == MC_EVENT_CHANNEL_CREATE && out.events[0].value == 2 &&
              equals(out.events[0].name, "voice") &&
              application_plugins_gateway_translate(deleted, {}, all, &out, collect) == 1 &&
              out.events[1].kind == MC_EVENT_CHANNEL_DELETE && out.events[1].guild_id == kGuild &&
              equals(out.events[1].name, "general"),
          "plugin events: channel events carry type and name, falling back to the cached channel");
    auto self_member = dispatch(GatewayClientDispatchKind::guild_member_update);
    self_member.state.guild_id = kGuild;
    self_member.state.id = kSelf;
    out = {};
    check(application_plugins_gateway_translate(self_member, {}, all, &out, collect) == 1 &&
              out.events[0].kind == MC_EVENT_MEMBER_UPDATE && (out.events[0].flags & MC_EVENT_OWN) &&
              equals(out.events[0].name, "Me"),
          "plugin events: your own member update is a member update with your name");
}

void release(AccountUiRuntime& ui) noexcept {
    message_rich_store_release(&ui.message_rich_store);
    user_activity_release(&ui.user_store.activities);
    user_appearance_release(&ui.user_store.appearance);
    g_app.message_view.store = {};
    relationships_ready = false;
}

} // namespace plugins_read_runtime

namespace app_detail {
const RelationshipStore* application_friends_relationships() noexcept {
    return plugins_read_runtime::relationships_ready ? &plugins_read_runtime::relationships : nullptr;
}
} // namespace app_detail

void test_application_plugins_read_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace plugins_read_runtime;
    auto* previous = g_app.account_ui;
    const auto saved_messages = g_app.message_view.store;
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(ui != nullptr, "plugin read: synthetic account state allocates");
    if (!ui) return;
    g_app.account_ui = ui;
    NativePluginSdkRuntime* runtime = static_cast<NativePluginSdkRuntime*>(
        VirtualAlloc(nullptr, sizeof(NativePluginSdkRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(runtime != nullptr, "plugin read: SDK runtime allocates");
    if (!runtime) {
        g_app.account_ui = previous;
        VirtualFree(ui, 0, MEM_RELEASE);
        return;
    }
    application_plugins_read_install(*runtime);
    const bool populated = populate(*ui);
    check(populated, populated ? "plugin read: synthetic stores populate" : populate_failure);
    if (populated) {
        test_reads(runtime->read, check);
        test_events(check);
    }
    release(*ui);
    VirtualFree(runtime, 0, MEM_RELEASE);
    g_app.account_ui = previous;
    g_app.message_view.store = saved_messages;
    VirtualFree(ui, 0, MEM_RELEASE);
}

} // namespace maicord

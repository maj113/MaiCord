// Random sequences of every chat state mutation that adds, removes or moves channel records, with the channel list
// check after each step. MAICORD_CHANNEL_LIST_FUZZ_SEEDS sets the number of seeds (the suite runs a few).
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild/display_order.hpp"
#include "../../support/chat_state_lists.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace channel_list_fuzz {

constexpr u32 kGuilds = 12, kChannels = 160, kNameBank = 1536, kRoles = 96, kOverwrites = 256;
constexpr u32 kGuildIds = 10, kChannelIds = 400, kOperations = 4000, kSuiteSeeds = 8;

struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[kGuilds];
    u16 guild_order[kGuilds];
    ChannelRecord channels[kChannels];
    u16 channel_order[kChannels];
    u8 names[kNameBank * 2];
    RoleRecord roles[kRoles];
    PermissionOverwriteRecord overwrites[kOverwrites];
};
Fixture fixture, before, mirror;

u32 failures;
u64 random_state;
u64 operations, rejected_batches, compactions;

u32 next(u32 bound) noexcept {
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return static_cast<u32>(((random_state * 0x2545F4914F6CDD1Dull) >> 32) % bound);
}

u64 guild_id() noexcept {
    return 1000 + next(kGuildIds);
}

const char kLetters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
char name_bytes[4096][48];
u32 name_cursor;

// A name that stays readable until 4,096 later names, longer than the inputs of one batch live.
Utf8View name() noexcept {
    char* text = name_bytes[name_cursor++ % 4096];
    const u32 bytes = next(4) == 0 ? 0 : 1 + next(40);
    for (u32 i = 0; i < bytes; ++i)
        text[i] = kLetters[next(sizeof(kLetters) - 1)];
    return {text, bytes};
}

ChannelInput channel_input(u64 id, u64 guild) noexcept {
    static constexpr u8 kGuildTypes[] = {0, 0, 2, 4, 5, 13, 15, 10, 11, 12};
    ChannelInput input{};
    input.id = id;
    input.guild_id = guild;
    input.name = name();
    input.name_known = true;
    input.type = guild ? kGuildTypes[next(sizeof(kGuildTypes))] : (next(2) ? 1 : 3);
    input.parent_id = guild && next(3) ? 1 + next(kChannelIds) : 0;
    input.position = next(50);
    input.last_message_known = next(2) != 0;
    input.last_message_id = input.last_message_known ? id + next(1000) : 0;
    if (channel_is_thread(input.type)) {
        input.thread_active_known = next(2) != 0;
        input.thread_listed_active = input.thread_active_known && next(2);
        input.thread_self_joined_known = next(2) != 0;
        input.thread_self_joined = input.thread_self_joined_known && next(2);
    }
    return input;
}

// The guild of a resident channel, else a random one or none (a private channel).
ChannelInput random_channel(bool keep_guild = true) noexcept {
    const u64 id = 1 + next(kChannelIds);
    const auto* resident = chat_state_store_find_channel(fixture.state, id);
    const u64 guild = resident && keep_guild && next(10) ? resident->guild_id : next(5) == 0 ? 0 : guild_id();
    return channel_input(id, guild);
}

bool read_channel(const void* context, u32 index, ChannelInput* output) noexcept {
    *output = static_cast<const ChannelInput*>(context)[index];
    return true;
}

bool read_role(const void* context, u32 index, RoleInput* output) noexcept {
    *output = static_cast<const RoleInput*>(context)[index];
    return true;
}

bool initialize() noexcept {
    reset_value(&fixture.state);
    return chat_state_store_initialize(&fixture.state, {fixture.guilds, kGuilds}, {fixture.guild_order, kGuilds},
                                       {fixture.channels, kChannels}, {fixture.channel_order, kChannels},
                                       {fixture.names, kNameBank * 2}) &&
           chat_state_store_bind_permissions(&fixture.state, {fixture.roles, kRoles},
                                             {fixture.overwrites, kOverwrites});
}

// A refused preflight must leave every byte of the store as it was.
bool unchanged() noexcept {
    return bytes_equal(&fixture, &before, sizeof(Fixture));
}

// The batch role insert on the store against role-by-role upserts on a copy: the same answer and the same bytes.
bool insert_roles_as_upserts(u64 guild, const RoleInput* roles, u32 count) noexcept {
    copy_bytes(&mirror, &fixture, sizeof(Fixture));
    const auto point = [](ChatStateStore& state, Fixture& arrays) noexcept {
        state.guilds = arrays.guilds;
        state.guild_order = arrays.guild_order;
        state.channels = arrays.channels;
        state.channel_order = arrays.channel_order;
        state.names = arrays.names;
        state.roles = arrays.roles;
        state.overwrites = arrays.overwrites;
    };
    point(mirror.state, mirror);
    bool upserted = true;
    for (u32 i = 0; upserted && i < count; ++i)
        upserted = chat_state_store_upsert_role(&mirror.state, roles[i]);
    const bool inserted = chat_state_store_insert_guild_roles(&fixture.state, guild, count, read_role, roles);
    point(mirror.state, fixture);
    constexpr u32 kArrays = static_cast<u32>(__builtin_offsetof(Fixture, guilds));
    return inserted == upserted && bytes_equal(&mirror.state, &fixture.state, sizeof(ChatStateStore)) &&
           bytes_equal(reinterpret_cast<const u8*>(&mirror) + kArrays, reinterpret_cast<const u8*>(&fixture) + kArrays,
                       sizeof(Fixture) - kArrays);
}

// GUILD_CREATE as the Gateway applies it: preflight, then the guild, its roles and its channels in packet order.
bool guild_create() noexcept {
    const u64 guild = guild_id();
    RoleInput roles[8]{};
    const u32 role_count = 1 + next(8);
    // The IDs repeat now and then: the batch insert then takes the role-by-role path.
    for (u32 i = 0; i < role_count; ++i)
        roles[i] = RoleInput{i ? guild * 100 + 1 + next(40) : guild, guild, next(2) ? ~0ull : 0, name(), i};
    ChannelInput channels[24]{};
    u32 channel_count = 0;
    // Packet IDs are unique: a repeated draw shortens the batch.
    for (u32 target = next(25); channel_count < target;) {
        const ChannelInput channel = channel_input(1 + next(kChannelIds), guild);
        bool repeated = false;
        for (u32 j = 0; j < channel_count; ++j)
            repeated = repeated || channels[j].id == channel.id;
        if (repeated)
            --target;
        else
            channels[channel_count++] = channel;
    }
    GuildInput input{};
    input.id = guild;
    input.name = name();
    input.kind = GuildMutationKind::create;
    input.owner_id = 1;
    copy_bytes(&before, &fixture, sizeof(Fixture));
    const auto admission = chat_state_store_preflight_guild_create(fixture.state, input, role_count, read_role,
                                                                   roles, channel_count, read_channel, channels);
    if (admission != ChatStateStoreError::none) {
        ++rejected_batches;
        return unchanged();
    }
    if (!chat_state_store_upsert_guild(&fixture.state, input).ok())
        return false;
    fixture.state.selection_held = true;
    chat_state_store_remove_guild_channels(&fixture.state, guild);
    chat_state_store_remove_guild_roles(&fixture.state, guild);
    if (!insert_roles_as_upserts(guild, roles, role_count))
        return false;
    for (u32 i = 0; i < channel_count; ++i)
        if (!chat_state_store_upsert_channel(&fixture.state, channels[i]).ok())
            return false;
    fixture.state.selection_held = false;
    chat_state_store_select_default(&fixture.state);
    chat_state_store_recompute_guild_permissions(&fixture.state, guild);
    return true;
}

// THREAD_LIST_SYNC as the Gateway applies it: one preflight for the batch, then each thread.
bool thread_batch() noexcept {
    const u64 guild = guild_id();
    ChannelInput threads[16]{};
    u32 count = 0;
    for (u32 i = next(17); count < i; ++count) {
        threads[count] = channel_input(1 + next(kChannelIds), guild);
        threads[count].type = static_cast<u8>(10 + next(3));
        threads[count].parent_id = 1 + next(kChannelIds);
        threads[count].thread_active_known = threads[count].thread_listed_active = true;
    }
    copy_bytes(&before, &fixture, sizeof(Fixture));
    if (chat_state_store_preflight_channel_batch(fixture.state, count, read_channel, threads) !=
        ChatStateStoreError::none) {
        ++rejected_batches;
        return unchanged();
    }
    for (u32 i = 0; i < count; ++i)
        if (!chat_state_store_upsert_channel(&fixture.state, threads[i]).ok())
            return false;
    return true;
}

// READY: the private channels and guilds go, the READY guilds arrive unavailable.
void ready() noexcept {
    chat_state_store_clear_private_channels(&fixture.state);
    chat_state_store_clear_guilds(&fixture.state);
    for (u32 i = next(kGuildIds); i; --i)
        chat_state_store_upsert_guild(&fixture.state, GuildInput{guild_id(), {}, GuildMutationKind::unavailable});
}

// The display order of the servers, without and with saved positions, names each live server one time by the
// slot that holds it, as the navigation rebuild and the startup shell read it.
GuildFolderSettings positions;
bool display_slots_valid(const GuildFolderSettings* settings) noexcept {
    GuildDisplayEntry entries[kGuilds];
    const u32 count = guild_display_order_build(settings, fixture.state, {entries, kGuilds});
    for (u32 i = 0; i < count; ++i)
        if (entries[i].slot >= kGuilds || !fixture.guilds[entries[i].slot].occupied ||
            fixture.guilds[entries[i].slot].id != entries[i].guild_id)
            return false;
    return count == fixture.state.guild_count;
}

// The IDs a reader of one guild sees in the list equal those of a walk over every channel in ID order.
bool readers_agree(u64 guild) noexcept {
    const ChatStateStore& state = fixture.state;
    const ChannelRecord* listed = chat_state_store_first_guild_channel(state, guild);
    for (u32 order = 0; order < state.channel_count; ++order) {
        const ChannelRecord& channel = state.channels[state.channel_order[order]];
        if (channel.guild_id != guild)
            continue;
        while (listed && listed->guild_id != guild)
            listed = chat_state_store_next_guild_channel(state, *listed);
        if (listed != &channel)
            return false;
        listed = chat_state_store_next_guild_channel(state, *listed);
    }
    while (listed && listed->guild_id != guild)
        listed = chat_state_store_next_guild_channel(state, *listed);
    return !listed;
}

bool step() noexcept {
    ChatStateStore& state = fixture.state;
    const u32 kind = next(100);
    if (kind < 30) {
        const ChannelInput input = random_channel();
        if (next(2) && chat_state_store_preflight_channel(state, input) != ChatStateStoreError::none)
            return true;
        chat_state_store_upsert_channel(&state, input);
    } else if (kind < 42) {
        chat_state_store_remove_channel(&state, 1 + next(kChannelIds));
    } else if (kind < 52) {
        GuildInput input{};
        input.id = guild_id();
        input.name = name();
        input.kind = next(3) ? GuildMutationKind::update : GuildMutationKind::unavailable;
        input.owner_id = 1;
        chat_state_store_upsert_guild(&state, input);
    } else if (kind < 57) {
        // One draw for each statement: argument order differs between the systems' compilers.
        const u64 guild = guild_id();
        chat_state_store_remove_guild(&state, guild, next(2) != 0);
    } else if (kind < 61) {
        chat_state_store_remove_guild_channels(&state, guild_id());
    } else if (kind < 70) {
        return guild_create();
    } else if (kind < 76) {
        return thread_batch();
    } else if (kind < 78) {
        chat_state_store_clear_private_channels(&state);
    } else if (kind < 79) {
        chat_state_store_clear_guilds(&state);
    } else if (kind < 81) {
        ready();
    } else if (kind < 82) {
        compactions += state.compactions;
        return initialize();
    } else if (kind < 86) {
        chat_state_store_invalidate_thread_authority(&state, next(4) ? guild_id() : 0);
    } else if (kind < 92) {
        chat_state_store_select_guild(&state, guild_id());
    } else if (kind < 94) {
        chat_state_store_select_home(&state);
    } else {
        const auto* channel = chat_state_store_find_channel(state, 1 + next(kChannelIds));
        if (channel && chat_state_store_append_channel_overwrite(
                           &state, channel->id, PermissionOverwriteInput{1 + next(5), next(2) ? 1024u : 0u, 0, 0}))
            chat_state_store_recompute_guild_permissions(&state, channel->guild_id);
    }
    return true;
}

bool run(u64 seed) noexcept {
    random_state = seed * 0x9E3779B97F4A7C15ull + 1;
    if (!initialize())
        return false;
    for (u32 operation = 0; operation < kOperations; ++operation) {
        ++operations;
        if (!step() || !chat_state_store_lists_valid(fixture.state) || !chat_state_store_names_counted(fixture.state) ||
            !readers_agree(next(3) ? guild_id() : 0) || !display_slots_valid(nullptr) ||
            !display_slots_valid(&positions)) {
            test_write_metric("channel list fuzz: failed seed ", seed);
            test_write_metric("channel list fuzz: failed operation ", operation);
            return false;
        }
    }
    compactions += fixture.state.compactions;
    return true;
}

u32 seeds() noexcept {
    wchar_t text[12];
    const u32 length = environment_variable(L"MAICORD_CHANNEL_LIST_FUZZ_SEEDS", text, 12);
    u32 value = 0;
    for (u32 i = 0; i < length && i < 9 && text[i] >= L'0' && text[i] <= L'9'; ++i)
        value = value * 10 + static_cast<u32>(text[i] - L'0');
    return value ? value : kSuiteSeeds;
}

} // namespace channel_list_fuzz

MAICORD_TEST_ENTRY(maicord_channel_list_fuzz_entry) {
    using namespace channel_list_fuzz;
    const u32 count = seeds();
    // Saved positions for seven of the ten server IDs, in a mixed order.
    positions.present = true;
    positions.position_count = 7;
    for (u32 i = 0; i < 7; ++i)
        positions.positions[i] = 1000 + (i * 3 + 1) % kGuildIds;
    bool passed = true;
    for (u32 seed = 1; passed && seed <= count; ++seed)
        passed = run(seed);
    test_check(&failures, passed, "channel list fuzz: each list matches the channel records after every step");
    test_check(&failures, compactions != 0 && rejected_batches != 0,
               "channel list fuzz: the sequences compact names and refuse batches");
    test_write_metric("channel list fuzz: operations ", operations);
    test_write_metric("channel list fuzz: refused batches ", rejected_batches);
    test_write_metric("channel list fuzz: name compactions ", compactions);
    test_exit(failures, "maicord_channel_list_fuzz");
}

} // namespace maicord

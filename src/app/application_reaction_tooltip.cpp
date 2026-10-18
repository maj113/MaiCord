#include "application_reaction_tooltip.hpp"
#include "application_message_history.hpp"
#include "application_state.hpp"
#include "application_channel_authority.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/protocol/reaction_users.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "maicord/ui/regional_flag.hpp"

namespace maicord::app_detail {
namespace {
// Long names are cut so three of them and the emoji name fit the tooltip.
constexpr u32 kNameBytes = 36;

enum class Phase : u8 {
    idle,
    required,
    submitted,
    ready,
    failed
};
struct ReactionTooltip {
    u64 scope, channel_id, message_id, emoji_id, epoch, channel_epoch;
    u32 count;
    MessageReactionType type;
    Phase phase;
    u8 attempts, emoji_name_bytes, name_count;
    u8 name_bytes[kReactionTooltipNames];
    char emoji_name[kMessageReactionMaximumEmojiNameBytes];
    char names[kReactionTooltipNames][kNameBytes + 3];
    // The offered label and its bold names; offer copies them.
    char label[kUiTooltipMaximumTextBytes];
    TextStyleSpan bold[kReactionTooltipNames];
};
static_assert(kReactionTooltipNames <= kUiTooltipMaximumBold);
ReactionTooltip hovered{};
bool source_current() noexcept {
    if (!g_app.account_ui)
        return false;
    const auto& ui = *g_app.account_ui;
    const u64 scope = application_account_scope();
    const auto found = message_store_find(g_app.message_view.store, hovered.message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    return hovered.scope == scope && hovered.channel_epoch == g_app.load_trace.channel_epoch &&
           hovered.channel_id == ui.active_channel_id &&
           hovered.channel_id == ui.chat_state.selected_channel_id &&
           application_channel_readable(g_app, hovered.channel_id) && message &&
           message->channel_id == hovered.channel_id;
}
void synchronize_source() noexcept {
    if (hovered.phase != Phase::idle && !source_current())
        hovered.phase = Phase::idle;
}

// The label for the hovered reaction: its name until the reactors are known.
Utf8View compose(Utf8View name, u32* bold_count) noexcept {
    auto& s = hovered;
    Utf8View names[kReactionTooltipNames]{};
    const u32 known = s.phase == Phase::ready ? s.name_count : 0;
    for (u32 i = 0; i < known; ++i)
        names[i] = {s.names[i], s.name_bytes[i]};
    const auto label =
        message_reaction_tooltip_label(s.emoji_id, name, {names, known}, s.count,
                                       {s.label, sizeof(s.label)}, {s.bold, kReactionTooltipNames});
    *bold_count = label.bold_count;
    return {s.label, label.bytes};
}

// Follows the hovered reaction; a different reaction or count needs its
// reactors fetched again.
void track(u64 scope, u64 channel_id, u64 message_id, u64 emoji_id, Utf8View name, MessageReactionType type,
           u32 count) noexcept {
    auto& s = hovered;
    if (s.phase != Phase::idle && s.scope == scope && s.channel_id == channel_id &&
        s.message_id == message_id && s.emoji_id == emoji_id && s.type == type && s.count == count &&
        s.channel_epoch == g_app.load_trace.channel_epoch &&
        utf8_equal({s.emoji_name, s.emoji_name_bytes}, name))
        return;
    const u64 epoch = s.epoch + 1;
    s = {};
    s.scope = scope;
    s.channel_id = channel_id;
    s.message_id = message_id;
    s.emoji_id = emoji_id;
    s.type = type;
    s.count = count;
    s.epoch = epoch;
    s.channel_epoch = g_app.load_trace.channel_epoch;
    copy_bytes(s.emoji_name, name.data, name.bytes);
    s.emoji_name_bytes = static_cast<u8>(name.bytes);
    s.phase = Phase::required;
}

// A display name cut on a character boundary, with an ellipsis when cut.
u8 copy_name(char* output, Utf8View name) noexcept {
    if (name.bytes <= kNameBytes) {
        copy_bytes(output, name.data, name.bytes);
        return static_cast<u8>(name.bytes);
    }
    u32 cut = kNameBytes;
    while (cut && (static_cast<u8>(name.data[cut]) & 0xC0) == 0x80)
        --cut;
    copy_bytes(output, name.data, cut);
    copy_bytes(output + cut, "\xE2\x80\xA6", 3);
    return static_cast<u8>(cut + 3);
}
} // namespace

bool application_reaction_tooltip_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    if (!target || !g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    const auto hit = message_scene_hit_test(g_app.message_scene, point.x, point.y);
    u32 index = 0;
    if (!message_scene_reaction_index(g_app.message_scene, hit, &index))
        return false;
    const auto found = message_store_find(g_app.message_view.store, hit.message_id);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto* rich = message_rich_store_find(ui.message_rich_store, hit.message_id);
    if (!record || record->channel_id != ui.active_channel_id || !rich ||
        rich->message_revision != record->revision || index >= rich->reaction_count)
        return false;
    const auto& reaction = rich->reactions[index];
    Utf8View name = message_rich_store_string(ui.message_rich_store, reaction.name);
    if (!name.bytes && reaction.emoji_id)
        if (const auto* emoji = chat_state_store_find_emoji(ui.chat_state, reaction.emoji_id))
            name = chat_state_store_emoji_name(*emoji);
    // The pill's own count: normal reactions, else super reactions.
    const auto type = reaction.normal_count ? MessageReactionType::normal : MessageReactionType::burst;
    const u32 count = reaction.normal_count ? reaction.normal_count : reaction.burst_count;
    const auto* node = message_scene_layout_node(g_app.message_scene, hit.widget);
    const auto anchor = node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{};
    if (!name.data || !name.bytes || name.bytes > kMessageReactionMaximumEmojiNameBytes || !count ||
        !ui_rect_has_area(anchor))
        return false;
    track(scope, record->channel_id, record->id, reaction.emoji_id, name, type, count);
    u32 bold_count = 0;
    const Utf8View label = compose(name, &bold_count);
    if (!label.bytes)
        return false;
    const u16 flag = reaction.emoji_id ? 0 : regional_flag_sequence(name);
    UiTooltipEmoji emoji{};
    if (reaction.emoji_id)
        emoji.media = media_asset_custom_emoji(reaction.emoji_id, (reaction.flags & 4u) != 0);
    else if (flag)
        emoji.media = media_asset_regional_flag(flag);
    else if (name.bytes <= kUiTooltipMaximumEmojiBytes)
        emoji.glyph = name;
    *target = {{{&g_app.message_scene.widgets, hit.widget}, scope, record->id, record->revision, 0x300u + index},
               anchor,
               label,
               UiTooltipPlacement::above,
               {hovered.bold, bold_count},
               emoji};
    return true;
}

void application_reaction_tooltip_leave() noexcept {
    // A submitted request remains owned by PRIMARY until it drains. Its
    // retired UI interest must not be recreated by the lane-reset recovery.
    hovered.phase = Phase::idle;
}

void application_reaction_tooltip_pump() noexcept {
    synchronize_source();
    if (hovered.phase == Phase::required &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none)
        pump_message_history();
}

bool application_reaction_tooltip_pending() noexcept {
    synchronize_source();
    return hovered.phase == Phase::required;
}

bool application_reaction_tooltip_query(ReactionTooltipQuery* query) noexcept {
    synchronize_source();
    auto& s = hovered;
    // Still submitted with the lane free: the lane was reset under the request.
    if (s.phase == Phase::submitted)
        s.phase = s.attempts < 2 ? Phase::required : Phase::failed;
    if (!query || s.phase != Phase::required)
        return false;
    *query = {{s.emoji_name, s.emoji_name_bytes}, s.channel_id, s.message_id, s.emoji_id, s.epoch, s.type};
    return true;
}

void application_reaction_tooltip_submitted(u64 epoch) noexcept {
    if (hovered.phase != Phase::required || hovered.epoch != epoch)
        return;
    hovered.phase = Phase::submitted;
    ++hovered.attempts;
}

void application_reaction_tooltip_failed() noexcept {
    if (hovered.phase == Phase::required || hovered.phase == Phase::submitted)
        hovered.phase = Phase::failed;
}

void application_reaction_tooltip_response(const RestClientResponse& response, ConstSlice<u8> body,
                                           Slice<u32> page_scratch, Slice<u32> object_scratch) noexcept {
    auto& s = hovered;
    // A response for a reaction no longer hovered: the new one is required.
    synchronize_source();
    if (s.phase != Phase::submitted)
        return;
    s.phase = Phase::failed;
    if (!response.transport_ok() || response.status_code != 200 || !body.data || !body.count ||
        !g_app.account_ui)
        return;
    ReactionUser users[kReactionTooltipNames]{};
    char text[1024];
    const auto decoded =
        decode_reaction_users({reinterpret_cast<const char*>(body.data), body.count}, page_scratch,
                              object_scratch, 16, {users, kReactionTooltipNames}, {text, sizeof(text)});
    if (!decoded.ok())
        return;
    // In a server, members are named as the member list names them.
    const auto& ui = *g_app.account_ui;
    const auto* channel = chat_state_store_find_channel(ui.chat_state, s.channel_id);
    const u64 guild_id = channel ? channel->guild_id : 0;
    for (u32 i = 0; i < decoded.user_count && i < kReactionTooltipNames; ++i) {
        const auto* member = guild_id ? guild_member_store_find(ui.guild_member_store, guild_id, users[i].id) : nullptr;
        const Utf8View nickname = member ? guild_member_store_nickname(*member) : Utf8View{};
        const Utf8View name = nickname.bytes ? nickname : users[i].display_name;
        if (!name.data || !name.bytes)
            continue;
        s.name_bytes[s.name_count] = copy_name(s.names[s.name_count], name);
        ++s.name_count;
    }
    s.phase = Phase::ready;
}
} // namespace maicord::app_detail

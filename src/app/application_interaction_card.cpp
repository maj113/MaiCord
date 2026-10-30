#include "application_interaction_card.hpp"
#include "application_account_presentation.hpp"
#include "application_state.hpp"
#include "application_tooltip.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/message_interaction_data.hpp"
#include "maicord/ui/message_interaction.hpp"

namespace maicord::app_detail {
namespace {
// The tooltip key of the pinned card (other tooltip actions use their own).
constexpr u32 kCardAction = 0x4943u;
// Operation of the account module's application_command entry: GET the
// interaction data of message `nonce` in `channel`.
constexpr u8 kInteractionDataOperation = 6;
constexpr float kCardMaximumWidth = 350.0f, kCardMaximumHeight = 240.0f;

enum class Phase : u8 { none, wanted, submitted, done };
struct Card {
    bool open;
    Phase phase;
    u64 scope, message_id, channel_id, submitted_id;
    // A press that closed the card this message: the click on the same chip is that close.
    u64 closed_by_press;
    u32 revision;
    u32 text_bytes;
    char text[kUiTooltipMaximumTextBytes];
};
Card g_card;
// The last message whose data arrived (of one account): opened again without a request.
struct Cache {
    u64 scope, message_id;
    u32 bytes;
    char text[kUiTooltipMaximumTextBytes];
};
Cache g_cache;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.message_scene.active &&
           g_card.scope == g_app.plugin_account_scope_serial;
}

const MessageSceneRowWidget* find_row(u64 message_id) noexcept {
    const auto& scene = g_app.message_scene;
    for (u32 i = 0; i < scene.count; ++i)
        if (scene.rows[i].message_id == message_id)
            return &scene.rows[i];
    return nullptr;
}

Utf8View resolve_name(void*, InteractionDataEntity entity, u64 id) noexcept {
    const auto& ui = *g_app.account_ui;
    switch (entity) {
    case InteractionDataEntity::user: {
        const auto* user = user_store_find(ui.user_store, id);
        return user ? user_store_name(ui.user_store, *user) : Utf8View{};
    }
    case InteractionDataEntity::channel: {
        const auto* channel = chat_state_store_find_channel(ui.chat_state, id);
        return channel ? chat_state_store_channel_name(ui.chat_state, *channel) : Utf8View{};
    }
    case InteractionDataEntity::role: {
        const auto* channel = chat_state_store_find_channel(ui.chat_state, g_card.channel_id);
        const auto* role = channel ? chat_state_store_find_role(ui.chat_state, channel->guild_id, id) : nullptr;
        return role ? chat_state_store_role_name(ui.chat_state, *role) : Utf8View{};
    }
    }
    return {};
}

void publish(Utf8View text) noexcept {
    g_card.text_bytes = minimum(text.bytes, static_cast<u32>(sizeof(g_card.text)));
    copy_bytes(g_card.text, text.data, g_card.text_bytes);
    ++g_card.revision;
    application_request_frame();
}
} // namespace

void application_interaction_card_close() noexcept {
    if (!g_card.open)
        return;
    g_card.open = false;
    g_card.phase = Phase::none;
    g_card.submitted_id = 0;
    application_request_frame();
}

void application_interaction_card_press() noexcept {
    g_card.closed_by_press = 0;
    if (!g_card.open)
        return;
    g_card.closed_by_press = g_card.message_id;
    application_interaction_card_close();
}

bool application_interaction_card_toggle(u64 message_id) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !message_id)
        return false;
    const auto* record = application_message_record(message_id);
    const auto* rich = record ? message_rich_store_find(g_app.account_ui->message_rich_store, record->id) : nullptr;
    if (!record || !rich || rich->message_revision != record->revision || !message_rich_has_interaction(*rich) ||
        !message_interaction_chip(record->type))
        return false;
    // The press of this very click closed the card of this chip.
    if (g_card.closed_by_press == message_id) {
        g_card.closed_by_press = 0;
        return true;
    }
    if (g_card.open && g_card.message_id == message_id) {
        application_interaction_card_close();
        return true;
    }
    const Utf8View name = message_rich_store_string(g_app.account_ui->message_rich_store, rich->interaction.command_name);
    if (!name.bytes)
        return false;
    g_card = Card{.open = true,
                  .phase = Phase::wanted,
                  .scope = g_app.plugin_account_scope_serial,
                  .message_id = message_id,
                  .channel_id = record->channel_id};
    char line[kUiTooltipMaximumTextBytes];
    const u32 bytes = minimum(name.bytes, static_cast<u32>(sizeof(line)) - 1);
    line[0] = '/';
    copy_bytes(line + 1, name.data, bytes);
    publish({line, bytes + 1});
    application_tooltip_wake();
    return true;
}

bool application_interaction_card_target(u64 scope, UiTooltipTarget* target, UiTooltipStyle* style) noexcept {
    if (!g_card.open || !target || !style)
        return false;
    const auto& renderer = g_app.renderer;
    UiRect anchor{};
    const auto* row = available() && scope && renderer.message_text && renderer.user_store
                          ? find_row(g_card.message_id)
                          : nullptr;
    if (!row || !message_interaction_command_anchor(g_app.message_scene, *renderer.message_text, *renderer.user_store,
                                                    renderer.message_measure_cache, application_theme(),
                                                    g_card.message_id, &anchor)) {
        application_interaction_card_close();
        return false;
    }
    *target = {{{&g_app.message_scene.widgets, row->reply}, scope, g_card.message_id, g_card.revision, kCardAction},
               anchor,
               {g_card.text, g_card.text_bytes},
               UiTooltipPlacement::above};
    const auto& scale = application_theme().scale;
    style->pointer_delay_ms = 0;
    style->padding_x = scale.space_4 + scale.space_2;
    style->padding_y = scale.space_4;
    style->maximum_width = kCardMaximumWidth;
    style->maximum_height = kCardMaximumHeight;
    return true;
}

bool application_interaction_card_pending(u64) noexcept {
    return g_card.open && g_card.phase == Phase::wanted;
}

bool application_interaction_card_submit(u64 request_id, u64) noexcept {
    if (!request_id || !application_interaction_card_pending(0))
        return false;
    if (!available() || !g_app.account.api.application_command) {
        g_card.phase = Phase::done;
        return false;
    }
    // A message read before shows its text again without a request.
    if (g_cache.scope == g_card.scope && g_cache.message_id == g_card.message_id && g_cache.bytes) {
        g_card.phase = Phase::done;
        publish({g_cache.text, g_cache.bytes});
        return false;
    }
    const auto result = g_app.account.api.application_command(g_app.account.instance, g_card.channel_id, 0, 0,
                                                              g_card.message_id, {}, request_id,
                                                              kInteractionDataOperation);
    if (!result.ok()) {
        // Busy retries on a later pump; a refused request keeps the bare command.
        if (result.error != RestClientSubmitError::busy)
            g_card.phase = Phase::done;
        return false;
    }
    g_card.phase = Phase::submitted;
    g_card.submitted_id = request_id;
    return true;
}

bool application_interaction_card_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_card.submitted_id || response.request_id != g_card.submitted_id)
        return false;
    g_card.submitted_id = 0;
    g_card.phase = Phase::done;
    if (!g_card.open || !available() || !response.transport_ok() || response.status_code != 200)
        return true;
    u32 scratch[kInteractionDataScratchStructurals];
    char text[kUiTooltipMaximumTextBytes];
    const InteractionDataNames names{nullptr, resolve_name};
    const u32 bytes = interaction_data_text(body, names, {scratch, kInteractionDataScratchStructurals}, {text, sizeof(text)});
    if (!bytes)
        return true;
    g_cache.scope = g_card.scope;
    g_cache.message_id = g_card.message_id;
    g_cache.bytes = bytes;
    copy_bytes(g_cache.text, text, bytes);
    publish({text, bytes});
    return true;
}
} // namespace maicord::app_detail

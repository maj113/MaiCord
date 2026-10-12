// Plugin message decorators (mc.msg.render@1) in the application: the bounded
// prepare pass that runs decorators before message text is rebuilt, the
// plugin-code-free query the message scene reads, session "Show" reveals and
// accessory clicks.
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_state.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/plugin_render.hpp"

namespace maicord::app_detail {
namespace {

constexpr u32 kRenderReveals = 64;
constexpr u32 kRenderScanLimit = kPluginSdkRenderEntries - 64;
struct RenderState {
    u64 reveals[kRenderReveals];
    u32 reveal_count, reveal_cursor;
    u64 reveal_revision;
    u64 prepared_signature; // inputs of the last pass that left nothing to evaluate
    u64 rendered_revision;  // published revision the message text was built for
    bool pending;           // more messages wait for evaluation
};
RenderState g_render;

NativePluginSdkRuntime* runtime() noexcept {
    return g_app.plugins && g_app.plugins->sdk ? &g_app.plugins->sdk->runtime : nullptr;
}
bool decorating(const NativePluginSdkRuntime* rt) noexcept {
    return rt && native_plugin_sdk_has_decorators(*rt);
}
bool revealed(u64 message_id) noexcept {
    for (u32 i = 0; i < g_render.reveal_count; ++i)
        if (g_render.reveals[i] == message_id)
            return true;
    return false;
}
// Revision the UI reads: decorations and reveals both change presentation.
u64 published(const NativePluginSdkRuntime* rt) noexcept {
    return decorating(rt) ? hash64_mix(hash64_mix(kHash64Seed, rt->render_revision), g_render.reveal_revision)
                          : g_render.reveal_revision;
}
void publish() noexcept { g_ui_plugin_render_revision = published(runtime()); }

UiPluginRenderNode node_view(const NativePluginSdkSlotNode& node) noexcept {
    return {node.owner_id, node.action_id, node.text, node.tooltip, node.value, node.icon, node.kind, node.tone};
}
// Installed for the UI: cached verdicts only, never a decorator call.
bool render_query(u64 message_id, u32 revision, UiPluginRenderDecoration* output) noexcept {
    auto* rt = runtime();
    if (!output || !decorating(rt))
        return false;
    NativePluginSdkRenderMessage message{};
    message.message_id = message_id;
    message.revision = revision;
    NativePluginSdkDecoration decoration{};
    if (!native_plugin_sdk_decorate(rt, message, false, &decoration))
        return false;
    *output = {};
    output->visibility = decoration.visibility;
    if (output->visibility == ui_plugin_render_collapsed && revealed(message_id))
        output->visibility = ui_plugin_render_normal;
    output->media = decoration.media;
    output->name_tone = decoration.name_tone;
    output->replace_content = decoration.replace_content;
    output->collapsed_label = decoration.collapsed_label;
    output->name_prefix = decoration.name_prefix;
    output->name_suffix = decoration.name_suffix;
    output->content = decoration.content;
    output->badge_count = minimum(decoration.badge_count, kUiPluginRenderBadges);
    output->accessory_count = minimum(decoration.accessory_count, kUiPluginRenderAccessories);
    for (u32 i = 0; i < output->badge_count; ++i)
        output->badges[i] = node_view(decoration.badges[i]);
    for (u32 i = 0; i < output->accessory_count; ++i)
        output->accessories[i] = node_view(decoration.accessories[i]);
    return true;
}

u32 relationship(u64 user_id) noexcept {
    const auto* store = application_friends_relationships();
    const auto* record = store && user_id ? relationship_store_find(*store, user_id) : nullptr;
    if (!record)
        return MC_RELATIONSHIP_NONE;
    switch (record->type) {
    case RelationshipType::friend_: return MC_RELATIONSHIP_FRIEND;
    case RelationshipType::blocked: return MC_RELATIONSHIP_BLOCKED;
    case RelationshipType::incoming: return MC_RELATIONSHIP_INCOMING;
    case RelationshipType::outgoing: return MC_RELATIONSHIP_OUTGOING;
    case RelationshipType::implicit: return MC_RELATIONSHIP_IMPLICIT;
    default: return MC_RELATIONSHIP_NONE;
    }
}

NativePluginSdkRenderMessage render_message(const AccountUiRuntime& ui, const MessageStore& store, MessageHandle handle,
                                            const MessageRecord& record) noexcept {
    NativePluginSdkRenderMessage message{};
    message.channel_id = record.channel_id;
    message.message_id = record.id;
    message.author_id = record.author_id;
    message.timestamp_ms = record.timestamp_ms;
    message.edited_ms = record.edited_timestamp_ms;
    message.revision = record.revision;
    if (const auto* channel = chat_state_store_find_channel(ui.chat_state, record.channel_id))
        message.guild_id = channel->guild_id;
    const auto* rich = message_rich_store_find(ui.message_rich_store, record.id);
    if (rich && rich->message_revision != record.revision)
        rich = nullptr;
    const auto* appearance = user_appearance_find(ui.user_store.appearance, record.author_id);
    u32 flags = 0;
    if (record.author_id && record.author_id == ui.chat_state.self_user_id)
        flags |= MC_MESSAGE_OWN;
    if (appearance && appearance->bot)
        flags |= MC_MESSAGE_BOT;
    if (record.edited_timestamp_ms)
        flags |= MC_MESSAGE_EDITED;
    if (record.presentation_flags & message_presentation_pinned)
        flags |= MC_MESSAGE_PINNED;
    if (rich && rich->reference.type != 1 && rich->reference.state != GatewayMessageReferenceState::none)
        flags |= MC_MESSAGE_REPLY;
    if (rich && (rich->attachment_count || rich->embed_count))
        flags |= MC_MESSAGE_MEDIA;
    message.flags = flags;
    message.relationship = relationship(record.author_id);
    message.attachment_count = rich ? rich->attachment_count : 0;
    message.embed_count = rich ? rich->embed_count : 0;
    if (record.content_availability == MessageContentAvailability::available)
        message.content = message_store_content(store, handle);
    if (const auto* user = user_store_find(ui.user_store, record.author_id))
        message.author_name = user_store_name(ui.user_store, *user);
    return message;
}

u64 inputs_signature(const NativePluginSdkRuntime& rt, const MessageStore& store) noexcept {
    u64 hash = hash64_mix(kHash64Seed, rt.render_revision);
    hash = hash64_mix(hash, store.created);
    hash = hash64_mix(hash, store.updated);
    hash = hash64_mix(hash, store.deleted);
    hash = hash64_mix(hash, store.count);
    if (store.count) {
        hash = hash64_mix(hash, store.records[store.order[0]].id);
        hash = hash64_mix(hash, store.records[store.order[store.count - 1]].id);
    }
    return hash;
}

} // namespace

void application_plugins_render_prepare() noexcept {
    // The query and the scene switch are cheap no-ops without decorators.
    g_ui_plugin_render_query = render_query;
    g_app.message_scene.plugin_decorations = true;
    auto* rt = runtime();
    if (!decorating(rt) || !g_app.account_ui || application_plugins_busy()) {
        g_render.pending = false;
        publish();
        return;
    }
    const AccountUiRuntime& ui = *g_app.account_ui;
    const MessageStore& store = g_app.message_view.store;
    if (!g_render.pending && g_render.prepared_signature == inputs_signature(*rt, store)) {
        publish();
        return;
    }
    u32 evaluated = 0, considered = 0;
    bool more = false;
    const auto visit = [&](MessageHandle handle) noexcept {
        const MessageRecord* record = message_store_resolve(store, handle);
        if (!record || !record->id)
            return true;
        if (++considered > kRenderScanLimit)
            return false;
        if (native_plugin_sdk_decoration_current(*rt, record->id, record->revision))
            return true;
        if (evaluated == kPluginSdkRenderPerPass) {
            more = true;
            return false;
        }
        NativePluginSdkDecoration ignored{};
        native_plugin_sdk_decorate(rt, render_message(ui, store, handle, *record), true, &ignored);
        ++evaluated;
        // A decorator may have unloaded its plugin (budget); stop the pass.
        return decorating(runtime());
    };
    // Laid-out rows first, then the rest of the resident window, newest first.
    bool going = true;
    const auto& scene = g_app.message_scene;
    for (u32 i = 0; going && scene.active && i < scene.count; ++i)
        going = visit(scene.rows[i].message);
    for (u32 i = store.count; going && i-- > 0;)
        going = visit(message_store_ordered_handle(store, i));
    rt = runtime();
    g_render.pending = more && decorating(rt);
    g_render.prepared_signature = rt ? inputs_signature(*rt, store) : 0;
    publish();
    if (g_render.pending)
        application_request_frame();
}

bool application_plugins_render_stale() noexcept {
    publish();
    return g_render.pending || g_render.rendered_revision != g_ui_plugin_render_revision;
}

void application_plugins_render_rendered() noexcept { g_render.rendered_revision = g_ui_plugin_render_revision; }

void application_plugins_render_poll() noexcept {
    const auto* rt = runtime();
    if (!g_app.account_ui)
        return;
    // Plugin invalidations (from timers and events) and decorators going away
    // both need a prepare pass and a text rebuild without other UI work.
    const bool changed = g_render.pending || published(rt) != g_render.rendered_revision ||
                         (decorating(rt) && g_render.prepared_signature != inputs_signature(*rt, g_app.message_view.store));
    if (changed && refresh_account_text(false))
        application_request_frame();
}

bool application_plugins_render_click(const MessageTextHit& hit, UiRect anchor) noexcept {
    if (!hit.valid || hit.kind != MessageMarkdownSemanticKind::link ||
        !ui_plugin_render_source(static_cast<u16>(hit.source_offset >> 16u)) || !hit.message_id)
        return false;
    const auto& store = g_app.message_view.store;
    const MessageStoreResult found = message_store_find(store, hit.message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record || record->revision != hit.message_revision)
        return true;
    if (hit.target == kUiPluginRenderShowTarget) {
        // Session reveal: the cached verdict stays; the query reports it shown.
        if (!revealed(record->id)) {
            if (g_render.reveal_count < kRenderReveals) {
                g_render.reveals[g_render.reveal_count++] = record->id;
            } else {
                g_render.reveals[g_render.reveal_cursor] = record->id;
                g_render.reveal_cursor = (g_render.reveal_cursor + 1) % kRenderReveals;
            }
            ++g_render.reveal_revision;
            publish();
            refresh_account_text(true);
            application_request_frame();
        }
        return true;
    }
    UiPluginRenderDecoration decoration{};
    if (!render_query(record->id, record->revision, &decoration) || !hit.target ||
        hit.target > decoration.accessory_count)
        return true;
    const UiPluginRenderNode node = decoration.accessories[hit.target - 1];
    if (node.kind == kUiPluginRenderLink)
        return node.value.bytes ? application_open_content_url(node.value) : true;
    auto* rt = runtime();
    if (node.kind != kUiPluginRenderButton || !rt || application_plugins_busy())
        return true;
    // A gesture inside input routing, never during layout or paint.
    native_plugin_sdk_render_action(rt, node.owner_id, record->channel_id, record->id, node.action_id);
    if (runtime() == rt && rt->popover_count > 0)
        application_plugins_popover_open(anchor);
    application_request_frame();
    return true;
}

} // namespace maicord::app_detail

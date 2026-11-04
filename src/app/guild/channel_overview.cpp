#include "channel_overview_internal.hpp"
#include "../account/user_actions.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "../account/application_account_module.hpp"
#include "../media/picker.hpp"
#include "../chat/composer/attachments.hpp"

namespace maicord::app_detail {

bool application_channel_overview_open(u64 channel_id) noexcept {
    if (!available() || !g_app.renderer_ready || g_app.text_form || application_channel_overview_active() ||
        (owner && (owner->controller.request_id || owner->request_id)) || lifetime == ~u64{0} ||
        !(manage(channel_id) || manage_channel(channel_id) || manage_overrides(channel_id)))
        return false;
    settings_modal_yield();
    if (!owner) {
        owner = settings_modal_allocate<Overview>(kHooks);
        if (!owner)
            return false;
        // User limit labels: "No limit", then the member count.
        Bank text{owner->user_limit_text, sizeof(owner->user_limit_text), 0};
        owner->user_limit_labels[0] = utf8_literal("No limit");
        for (u32 i = 1; i <= kChannelSettingsUserLimitMaximum; ++i) {
            const u32 start = text.used;
            text.number(i);
            text.add(i == 1 ? utf8_literal(" user") : utf8_literal(" users"));
            owner->user_limit_labels[i] = {owner->user_limit_text + start, text.used - start};
        }
    }
    auto& p = *owner;
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    p.lifetime = ++lifetime;
    p.permission_observed = false;
    p.identity = scope(channel_id);
    p.type = channel->type;
    p.text = channel->type == 0 && manage(channel_id);
    p.can_overview = p.text || (channel_settings_supported(p.type) && manage_channel(channel_id));
    p.can_permissions = manage_overrides(channel_id);
    p.can_delete = manage_channel(channel_id);
    p.loaded = p.fetch_failed = p.conflict = p.echo = false;
    p.op = Op::none;
    p.queued = p.failed = false;
    p.request_id = 0;
    reset_value(&p.status);
    p.menu = Menu::none;
    p.selected_target = channel->guild_id;
    settings_layer_reset(&p.layer);
    for (auto& model : p.models)
        message_composer_release(&model);
    if (p.text) {
        if (!channel_overview_controller_open(&p.controller, p.identity, true, true))
            return false;
        message_composer_initialize(&p.models[0], {p.controller.draft.name, kChannelOverviewNameBytes});
        message_composer_initialize(&p.models[1], {p.controller.draft.topic, kChannelOverviewTopicBytes});
    } else {
        reset_value(&p.controller);
        clear_bytes(&p.base, sizeof(p.base));
        clear_bytes(&p.draft, sizeof(p.draft));
        p.draft.channel_id = channel_id;
        message_composer_initialize(&p.models[0], {p.draft.name, kGuildAdminNameBytes});
    }
    load_overwrites();
    if (!import_models()) {
        application_channel_overview_close(false);
        return false;
    }
    p.content = {utf8_literal("Overview"), Utf8View{}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle,
                 &p.footer};
    p.permissions_content = {utf8_literal("Permissions"), Utf8View{}, {}, 1, kUiSettingsPageFormat,
                             &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_initialize(&p.registry, {p.pages, 2}) ||
        !ui_page_registry_register(&p.registry, {page_key, utf8_literal("Overview"), &p.content}).ok() ||
        !ui_page_registry_register(&p.registry, {permissions_key, utf8_literal("Permissions"), &p.permissions_content})
             .ok()) {
        application_channel_overview_close(false);
        return false;
    }
    ui_page_registry_select(&p.registry, p.can_overview ? page_key : permissions_key);
    if (!p.text && p.can_overview)
        queue(Op::fetch);
    settings_modal_take_input();
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::layer);
    build_navigation();
    // The first update activates the scene; project() fills the page after.
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_channel_overview_close(false);
        return false;
    }
    settings_modal_bind(p);
    application_sync_attachment_drop_target();
    synchronize_member_subscription(clock_monotonic_milliseconds());
    if (repaint())
        return true;
    application_channel_overview_close(false);
    return false;
}
void application_channel_overview_close(bool restore_focus) noexcept {
    if (const auto* modal = active_modal()) {
        modal->close(restore_focus);
        return;
    }
    if (!owner || (!owner->scene.active && !owner->controller.active))
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&owner->layer);
    if (owner->scene.active)
        settings_layer_cancel_pointer(&owner->layer);
    application_text_form_cancel_owner(form_valid);
    channel_overview_controller_cancel(&owner->controller);
    owner->queued = false;
    settings_modal_unbind(*owner);
    owner->layer.blink_at = owner->layer.shake_started = 0;
    owner->permission_observed = false;
    settings_modal_restore_focus(restore_focus && available());
    synchronize_member_subscription(clock_monotonic_milliseconds());
    settings_modal_repaint_shell();
}
void application_channel_overview_release() noexcept {
    for (const auto& modal : kModals)
        modal.release();
    application_user_actions_release();
    application_channel_overview_close(false);
    if (!owner)
        return;
    for (auto& model : owner->models)
        message_composer_release(&model);
    settings_modal_free(owner);
}
bool application_channel_overview_synchronize() noexcept {
    if (const auto* modal = active_modal())
        return modal->synchronize();
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    auto& c = p.controller;
    const auto current = scope(p.identity.channel_id);
    if (!channel_overview_scope_equal(current, p.identity) || !g_app.account_ui || g_app.login.visible ||
        !(manage(current.channel_id) || manage_channel(current.channel_id) || manage_overrides(current.channel_id) ||
          (p.text && c.active))) {
        application_text_form_cancel_owner(form_valid);
        application_channel_overview_close(false);
        return true;
    }
    bool changed = false;
    const bool permissions = manage_overrides(current.channel_id), deletable = manage_channel(current.channel_id);
    if (permissions != p.can_permissions || deletable != p.can_delete) {
        p.can_permissions = permissions;
        p.can_delete = deletable;
        build_navigation();
        changed = true;
    }
    if (p.text) {
        const auto phase = c.phase;
        const bool online = c.available, permission = c.can_manage;
        if (p.permission_observed) {
            p.permission_observed = false;
            channel_overview_controller_reauthorize(&c, current, available(), manage(current.channel_id));
        } else
            channel_overview_controller_authority(&c, current, available(), manage(current.channel_id));
        changed = changed || phase != c.phase || online != c.available || permission != c.can_manage ||
                  p.projected_revision != c.revision;
    }
    if (settings_layer_choice_lapsed(p.layer)) {
        p.menu = Menu::none;
        changed = true;
    }
    changed = reconcile() || changed;
    if (changed) {
        if (!editable())
            application_composer_text_input_suspend();
        return repaint();
    }
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
bool application_channel_overview_submit(u64 now) noexcept {
    for (const auto& modal : kModals)
        if (modal.next_wake && modal.next_wake())
            return modal.submit(now);
    if (const u64 social = application_user_actions_next_wake(); social && social <= now &&
                                                                application_user_actions_submit(now))
        return true;
    if (!owner || g_app.details_request_owner != MessageHistoryRequestOwner::none ||
        !application_channel_overview_synchronize() || !owner->scene.active)
        return false;
    auto& p = *owner;
    if (p.text) {
        const auto request = channel_overview_controller_request(p.controller, now);
        if (request.ready && g_app.account.api.channel_overview && g_app.account.instance) {
            const u64 id = application_next_request_id();
            const auto submitted = g_app.account.api.channel_overview(
                g_app.account.instance, request.operation, request.scope.channel_id,
                request.operation == ChannelOverviewOperation::update ? &request.patch : nullptr, id);
            if (!submitted.ok()) {
                if (submitted.error == RestClientSubmitError::busy)
                    p.controller.retry_at_ms = deadline_after(now, 50);
                else {
                    channel_overview_controller_admission_failed(&p.controller, request,
                                                                 ChannelOverviewFailure::rejected);
                    repaint();
                }
                return true;
            }
            if (!channel_overview_controller_submitted(&p.controller, request, id))
                return false;
            g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
            g_app.details_request_id = id;
            repaint();
            return true;
        }
    }
    if (!p.queued || p.request_id || (p.retry_at && p.retry_at > now) || !g_app.account.instance)
        return false;
    const u64 id = application_next_request_id();
    RestClientSubmitResult submitted{RestClientSubmitError::invalid_input};
    // Channel routes are channel-major: no guild ID.
    GuildAdminRequest request{};
    request.channel_id = p.identity.channel_id;
    ChannelPatch patch{};
    static PermissionOverwriteWrite overwrites[kPermissionOverwriteDraftMaximum]; // UI-thread scratch; written before read
    bool valid = true;
    if (p.op == Op::fetch) {
        if (g_app.account.api.channel_overview)
            submitted = g_app.account.api.channel_overview(g_app.account.instance, ChannelOverviewOperation::fetch,
                                                           p.identity.channel_id, nullptr, id);
    } else {
        if (p.op == Op::update) {
            request.operation = GuildAdminOperation::channel_update;
            valid = channel_settings_patch(p.base, p.draft, &patch);
            request.channel = &patch;
        } else if (p.op == Op::overwrites) {
            request.operation = GuildAdminOperation::channel_update;
            const u32 count = permission_overwrite_draft_write(p.overwrite_draft, {overwrites, kPermissionOverwriteDraftMaximum});
            valid = count != ~0u;
            patch.fields = channel_patch_overwrites;
            patch.overwrites = {overwrites, valid ? count : 0};
            request.channel = &patch;
        } else
            request.operation = GuildAdminOperation::channel_delete;
        if (valid && g_app.account.api.guild_admin)
            submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, id);
    }
    if (submit_busy(submitted, p.retry_at, now, 50))
        return false;
    if (!submitted.ok()) {
        p.queued = false;
        p.failed = true;
        p.status = utf8_literal("This change couldn't be sent.");
        if (p.op == Op::fetch)
            p.fetch_failed = true;
        repaint();
        return false;
    }
    p.queued = false;
    p.request_id = id;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    repaint();
    return true;
}
bool application_channel_overview_response(const RestClientResponse& response, Utf8View body) noexcept {
    for (const auto& modal : kModals)
        if (modal.owns_response && modal.owns_response(response.request_id))
            return modal.response(response, body);
    if (application_user_actions_owns_response(response.request_id))
        return application_user_actions_response(response, body);
    if (!owner)
        return false;
    auto& p = *owner;
    if (p.request_id && p.request_id == response.request_id) {
        p.request_id = 0;
        const bool ok = response.succeeded();
        if (!p.scene.active || !channel_overview_scope_equal(scope(p.identity.channel_id), p.identity))
            return true;
        const u64 now = clock_monotonic_milliseconds();
        if (!ok) {
            p.failed = true;
            p.status = response.error != RestClientError::none ? utf8_literal("Couldn't reach Discord. Try again.")
                       : response.status_code == 403 ? utf8_literal("You don't have permission to do this.")
                       : response.status_code == 429 ? utf8_literal("Too many changes at once. Try again shortly.")
                                                     : utf8_literal("Discord rejected this change.");
            if (p.op == Op::fetch)
                p.fetch_failed = true;
            return repaint();
        }
        if (p.op == Op::fetch) {
            // UI-thread scratch (a draft is over 4 KiB): a stack copy would cost the response a stack probe.
            static ChannelSettingsDraft fetched;
            reset_value(&fetched);
            if (channel_settings_decode(body, &fetched) && fetched.channel_id == p.identity.channel_id) {
                if (p.loaded && !channel_settings_equal(p.base, p.draft) && !channel_settings_equal(fetched, p.base))
                    p.conflict = true;
                else {
                    copy_bytes(&p.draft, &fetched, sizeof(fetched));
                    copy_bytes(&p.base, &fetched, sizeof(fetched));
                    p.loaded = true;
                    p.conflict = false;
                    import_models();
                }
            } else {
                p.fetch_failed = true;
                p.status = utf8_literal("The channel details couldn't be read.");
            }
        } else if (p.op == Op::update) {
            copy_bytes(&p.base, &p.draft, sizeof(p.base));
            p.echo = true;
            p.echo_until = deadline_after(now, kEchoMs);
        } else if (p.op == Op::overwrites) {
            p.overwrite_base = p.overwrite_draft;
            p.overwrite_echo = true;
            p.overwrite_echo_until = deadline_after(now, kEchoMs);
        } else if (p.op == Op::remove) {
            application_channel_overview_close();
            return true;
        }
        return repaint();
    }
    if (p.controller.request_id != response.request_id)
        return false;
    if (!application_channel_overview_synchronize())
        return false;
    ChannelOverviewCompletion completion{ChannelOverviewCompletionKind::ambiguous, 0};
    static ChannelOverviewSnapshot remote;
    reset_value(&remote);
    bool decoded = false;
    if (response.transport_ok()) {
        if (response.status_code == 403 || response.status_code == 404)
            completion.kind = ChannelOverviewCompletionKind::permission_lost;
        else if (response.status_code == 429) {
            completion.kind = ChannelOverviewCompletionKind::rate_limited;
            completion.retry_at_ms = response.retry_at_ms;
        } else if (response.status_code >= 400 && response.status_code < 500)
            completion.kind = ChannelOverviewCompletionKind::rejected;
        else if (response.status_code == 200) {
            completion.kind = ChannelOverviewCompletionKind::success;
            decoded = channel_overview_decode(body, &remote).ok();
        }
    }
    if (!channel_overview_controller_complete(&p.controller, p.controller.scope, response.request_id, completion,
                                              decoded ? &remote : nullptr, clock_monotonic_milliseconds()))
        return false;
    if (!p.controller.active)
        return true;
    return import_models() && repaint();
}
void application_channel_overview_gateway(const GatewayClientEvent& event, Utf8View body) noexcept {
    application_guild_settings_gateway(event, body);
    application_media_picker_gateway(event, body);
    if (!owner || !owner->controller.active)
        return;
    if (event.kind == GatewayClientEventKind::ready) {
        application_channel_overview_close(false);
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    // The mailbox contains pointer-free model events, not raw channel JSON.
    // Refetch omitted metadata and invalidate an older outstanding REST request.
    if (event.dispatch_kind == GatewayClientDispatchKind::channel_update) {
        if (event.state.id == owner->controller.scope.channel_id &&
            event.state.guild_id == owner->controller.scope.guild_id) {
            channel_overview_controller_invalidate(&owner->controller, owner->controller.scope);
            if (event.state.fields & gateway_client_state_field_permission_overwrites)
                owner->permission_observed = true;
        }
    }
    const bool role_permissions = (event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
                                   event.dispatch_kind == GatewayClientDispatchKind::guild_role_update) &&
                                  (event.state.fields & gateway_client_state_field_permissions);
    const bool permission_event = role_permissions ||
                                  event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete ||
                                  (event.dispatch_kind == GatewayClientDispatchKind::guild_member_update &&
                                   event.state.id == owner->controller.scope.self_user_id);
    if (permission_event && event.state.guild_id == owner->controller.scope.guild_id)
        owner->permission_observed = true;
}
// The topmost active modal's layer, in the order the modals stack; null when none is open.
static SettingsLayer* modal_layer() noexcept {
    const auto* modal = active_modal();
    return modal ? modal->layer() : nullptr;
}
// The layer text editing goes to. The report has no editor and the profile editor none under a dialog.
static SettingsLayer* edit_layer() noexcept {
    auto* layer = modal_layer();
    if (!layer)
        return owner && owner->scene.active ? &owner->layer : nullptr;
    return layer == application_report_settings_layer()           ? nullptr
           : layer == application_profile_editor_settings_layer() ? application_profile_editor_edit_layer()
                                                                  : layer;
}
bool application_channel_overview_edit_focused() noexcept {
    auto* layer = edit_layer();
    return layer && settings_layer_editor(layer, nullptr, nullptr, nullptr);
}
MessageComposer* application_channel_overview_edit_model() noexcept {
    MessageComposer* model = nullptr;
    auto* layer = edit_layer();
    return layer && settings_layer_editor(layer, nullptr, &model, nullptr) ? model : nullptr;
}
TextInputTarget application_channel_overview_edit_target() noexcept {
    BoundedEdit* binding = nullptr;
    auto* layer = edit_layer();
    return layer && settings_layer_editor(layer, nullptr, nullptr, &binding)
               ? bounded_edit_input_target(binding)
               : TextInputTarget{};
}
void application_channel_overview_edit_cancel_native_group(TextInputTarget target) noexcept {
    for (const auto& modal : kModals)
        if (auto* layer = modal.layer())
            settings_layer_cancel_native_group(layer, target);
    if (owner)
        settings_layer_cancel_native_group(&owner->layer, target);
}
UiRect application_channel_overview_edit_bounds() noexcept {
    auto* layer = edit_layer();
    return layer ? settings_layer_edit_bounds(layer) : UiRect{};
}
bool application_channel_overview_edit_changed(bool content) noexcept {
    auto* layer = edit_layer();
    return layer && settings_layer_edit_changed(layer, content);
}
u64 application_channel_overview_next_wake() noexcept {
    for (const auto& modal : kModals)
        if (modal.next_wake)
            if (const u64 wake = modal.next_wake())
                return wake;
    // User notes wait out a typing pause; never let that hide the modal's work.
    const u64 social = application_user_actions_next_wake();
    if (!owner || g_app.details_request_owner != MessageHistoryRequestOwner::none)
        return social;
    const u64 controller = channel_overview_controller_next_wake(owner->controller);
    const u64 own = owner->queued ? (owner->retry_at ? owner->retry_at : 1) : 0;
    const u64 wake = controller && own ? minimum(controller, own) : controller ? controller : own;
    return wake && social ? minimum(wake, social) : wake ? wake : social;
}
u64 application_channel_overview_caret_wake() noexcept {
    if (const auto* modal = active_modal())
        return modal->caret_wake();
    return owner && owner->scene.active ? settings_layer_next_wake(owner->layer) : 0;
}
void application_channel_overview_tick(u64 now) noexcept {
    application_guild_settings_tick(now);
    // Server Settings ticks its own layer.
    const auto* own = application_guild_settings_settings_layer();
    const auto tick = [&](SettingsLayer* layer) noexcept {
        if (layer && layer != own && layer->scene && layer->scene->active)
            settings_layer_tick(layer, now);
    };
    for (const auto& modal : kModals)
        tick(modal.layer());
    tick(owner ? &owner->layer : nullptr);
}
bool application_channel_overview_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* layer = modal_layer();
    if (layer && layer == application_profile_editor_settings_layer())
        return application_profile_editor_input(kind, key, repeated);
    if (!layer && owner)
        layer = &owner->layer;
    return layer && settings_layer_input(layer, kind, key, repeated);
}

SettingsLayer* application_channel_overview_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

bool application_channel_overview_window_event(const WindowEvent& event) noexcept {
    auto* layer = modal_layer();
    if (layer && layer == application_profile_editor_settings_layer())
        return application_profile_editor_window_event(event);
    if (!layer && owner && owner->scene.active)
        layer = &owner->layer;
    return layer && settings_layer_window_event(layer, event);
}

} // namespace maicord::app_detail

#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_plugins.hpp"
#include "application_message_interaction.hpp"
#include "application_popup_plugin_owner.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/navigation_context.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"

// Plugin rows in the native user, channel, DM and server menus, through the
// real SDK registry (an in-process ABI-5 plugin), the shipping projection,
// binder and dispatcher, and the actual navigation scenes.
namespace maicord {
namespace plugin_navigation_runtime {
using namespace app_detail;
constexpr u64 owner = 0x4D434E41564D454Eull;
constexpr u64 kSelf = 500, kFriend = 501;
struct Fixture {
    GuildRecord guilds[2]; ChannelRecord channels[4];
    RoleRecord role; PermissionOverwriteRecord overwrite;
    u16 guild_order[2],channel_order[4]; u8 names[2048];
    GuildViewRow guild_rows[2]; ChannelViewRow channel_rows[2];
    GuildRailRowWidget rail_rows[2]; UiWidget rail_widgets[4]; UiLayoutNode rail_nodes[4];
    ShellNavigationRowWidget navigation_rows[2]; UiWidget navigation_widgets[10]; UiLayoutNode navigation_nodes[10];
    PopupItem popup_items[16]; PopupRowWidget popup_rows[16]; UiWidget popup_widgets[34]; UiLayoutNode popup_nodes[34];
    PrivateChannelStore private_channels; PrivateChannelRecord private_records[2]; u64 recipients[4];
    NativePluginSdkBinding* binding;
    void* plugin_binding; u64 generation;
    const mc_menus_v2* menus;
    mc_menu_context last;
    u32 callbacks;
    bool dismissed, invalidate_in_callback, account_owned;
} fixture{};
NativePluginSdkRuntime runtime;
alignas(64) u8 plugin_state[4096];

mc_status MC_CALL handler(void*, uint32_t, const mc_menu_context* context) {
    auto& f=fixture;++f.callbacks;f.last=*context;
    f.dismissed=!g_app.popup_scene.active && !application_plugins_menu_state().count;
    if(f.invalidate_in_callback)application_plugins_account_scope_invalidated();
    return MC_OK;
}
mc_status MC_CALL start(const mc_host* host, void*, uint32_t) {
    auto& f=fixture;const void* found=nullptr;
    f.plugin_binding=host->binding;f.generation=host->generation;
    if(host->get_interface(host->binding,host->generation,"mc.ui.menus",2,&found)!=MC_OK)return MC_E_INVALID;
    f.menus=static_cast<const mc_menus_v2*>(found);
    const auto add=[&](u64 id,u32 target,const char* label,u32 bytes,u32 flags){
        return f.menus->register_item(host->binding,host->generation,id,target,{label,bytes},flags)==MC_OK;};
    return f.menus->set_handler(host->binding,host->generation,handler)==MC_OK &&
        add(1,MC_MENU_CHANNEL,"Open channel options",20,0) &&
        add(2,MC_MENU_GUILD,"Remove server data",18,MC_MENU_ITEM_DANGER) &&
        add(3,MC_MENU_CHANNEL,"Unavailable channel action",26,MC_MENU_ITEM_DISABLED) &&
        add(4,MC_MENU_USER,"Inspect user",12,0) ? MC_OK : MC_E_INVALID;
}
void MC_CALL stop(void*, uint32_t) {}
mc_status MC_CALL query(const mc_host_info*, mc_plugin_definition* definition) {
    mc_plugin_definition local{};
    local.size=sizeof(local);local.abi_version=MC_ABI_VERSION;local.owner_id=owner;local.permissions=MC_PERM_MENUS;
    local.state_bytes=64;local.state_alignment=16;local.min_host_abi=5;local.start=start;local.stop=stop;
    return mc_definition_publish(definition,&local);
}
bool enable() noexcept {
    auto& f=fixture;auto* host=application_settings_plugin_host();
    if(!host)return false;
    host->sdk=&runtime;
    NativePluginDefinition adapter{};
    f.binding=native_plugin_sdk_create(&runtime,MC_PERM_ALL);
    return f.binding && native_plugin_sdk_query(f.binding,query,owner,&adapter)==NativePluginSdkQueryError::none &&
        adapter.state_bytes<=sizeof(plugin_state) && native_plugin_sdk_enable(f.binding,host,adapter,{plugin_state,adapter.state_bytes}).ok();
}
bool disable() noexcept {
    auto& f=fixture;
    const bool disabled=native_plugin_host_disable(application_settings_plugin_host_current(),owner).ok();
    const bool destroyed=f.binding && native_plugin_sdk_destroy(f.binding);
    f.binding=nullptr;
    return disabled && destroyed;
}
bool initialize() noexcept {
    auto& f=fixture;
    if(g_app.account_ui || g_app.settings || g_app.plugins)return false;
    g_app.account_ui=static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr,sizeof(AccountUiRuntime),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!g_app.account_ui)return false;
    f.account_owned=true;
    g_app.account.status.profile_id=77;g_app.login.visible=false;g_app.renderer_ready=true;g_app.renderer.dpi=96;
    g_app.context_focus=nullptr;g_app.text_form=nullptr;
    application_plugins_account_scope_invalidated();
    auto& state=g_app.account_ui->chat_state;auto& view=g_app.account_ui->navigation_view;
    if(!chat_state_store_initialize(&state,{f.guilds,2},{f.guild_order,2},{f.channels,4},{f.channel_order,4},{f.names,sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&state,{7,utf8_literal("Server"),GuildMutationKind::create,1}).ok())return false;
    for(u32 index=0;index<2;++index) {
        ChannelInput input{};input.id=88+index;input.guild_id=7;input.type=index?2:0;
        input.name=utf8_literal("Navigation target");input.name_known=true;
        if(!chat_state_store_upsert_channel(&state,input).ok())return false;
        auto* record=const_cast<ChannelRecord*>(chat_state_store_find_channel(state,input.id));
        record->permissions_resolved=true;record->effective_permissions=kPermissionViewChannel|kPermissionReadMessageHistory;
        record->actions=channel_action_view|channel_action_read_history;
        f.channel_rows[index]={};f.channel_rows[index].id=input.id;f.channel_rows[index].type=input.type;f.channel_rows[index].selectable=true;
        f.channel_rows[index].revision=record->revision;f.channel_rows[index].generation=record->generation;
    }
    const u64 friend_id=kFriend;
    if(!private_channel_store_initialize(&f.private_channels,{f.private_records,2},{f.recipients,4}) ||
        !private_channel_store_replace(&f.private_channels,{88,0,{&friend_id,1},{},true}))return false;
    state.private_channels=&f.private_channels;state.self_user_id=kSelf;
    state.selected_guild_id=7;state.selected_channel_id=88;
    state.roles=&f.role;state.overwrites=&f.overwrite;
    view={};view.guild_rows=f.guild_rows;view.guild_capacity=view.guild_count=1;
    view.channel_rows=f.channel_rows;view.channel_capacity=view.channel_count=2;
    view.guild_revision=view.channel_revision=1;view.selected_guild_id=7;view.selected_channel_id=88;
    f.guild_rows[0]={};f.guild_rows[0].id=7;
    return guild_rail_scene_initialize(&g_app.guild_rail_scene,{f.rail_rows,2},{f.rail_widgets,4},{f.rail_nodes,4}) &&
        guild_rail_scene_update(&g_app.guild_rail_scene,&view,{0,48,72,700},0).ok() &&
        shell_navigation_scene_initialize(&g_app.shell_navigation_scene,{f.navigation_rows,2},{f.navigation_widgets,10},{f.navigation_nodes,10}) &&
        shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() &&
        popup_scene_initialize(&g_app.popup_scene,{f.popup_items,16},{f.popup_rows,16},{f.popup_widgets,34},{f.popup_nodes,34}) && enable();
}
// Native rows, then the projected plugin rows, then the shipping owner
// transition and binder, as the real menu builders do.
bool open(ApplicationPluginMenuSurface surface,u64 id,u32 expected,bool bind=true,u64 guild=0) noexcept {
    auto& state=g_app.account_ui->chat_state;
    PopupItem items[12]{};
    const u32 builtins=surface==ApplicationPluginMenuSurface::guild?guild_context_policy_build(state,id,19,{items,1}):
        surface==ApplicationPluginMenuSurface::channel?channel_context_policy_build(state,id,17,18,{items,2}):0;
    const u32 extra=application_plugins_menu_items(surface,id,{items+builtins,8},guild);
    if(extra!=expected)return false;
    if(!builtins)items[0].flags &= ~popup_item_separator_before;
    if (!popup_scene_open(&g_app.popup_scene,PopupKind::context_menu,id,{items,builtins+extra},
        {40,60,41,61},{0,0,1000,700},240).ok()) return false;
    const auto* channel=surface==ApplicationPluginMenuSurface::channel?chat_state_store_find_channel(state,id):nullptr;
    const auto next=surface==ApplicationPluginMenuSurface::guild?PopupOwnerKind::guild:
        surface==ApplicationPluginMenuSurface::user?PopupOwnerKind::member:
        channel&&channel->type==1?PopupOwnerKind::private_channel:PopupOwnerKind::channel;
    application_popup_plugin_owner_transition(next);
    g_app.popup_owner_kind=next;
    return !bind || application_plugins_menu_bind();
}
u32 first_plugin_row() noexcept {
    for(u32 i=0;i<g_app.popup_scene.item_count;++i)
        if(g_app.popup_scene.items[i].action==kApplicationPluginPopupActionFirst)return i;
    return ~0u;
}
void cleanup() noexcept {
    if(!fixture.account_owned)return;
    dismiss_popup();
    if(fixture.binding)disable();
    application_settings_release();popup_scene_release(&g_app.popup_scene);g_app.popup_scene={};
    g_app.guild_rail_scene={};g_app.shell_navigation_scene={};g_app.context_focus=nullptr;
    if(g_app.account_ui)VirtualFree(g_app.account_ui,0,MEM_RELEASE);g_app.account_ui=nullptr;fixture.account_owned=false;
}
} // namespace plugin_navigation_runtime
void test_application_plugin_navigation_runtime(void (*check)(bool,const char*) noexcept) noexcept {
    using namespace plugin_navigation_runtime;using namespace app_detail;using S=ApplicationPluginMenuSurface;
    const bool initialized=initialize();check(initialized,"navigation runtime: SDK plugin, navigation scenes and popup initialize");
    if(!initialized){cleanup();return;}
    auto& state=g_app.account_ui->chat_state;auto& view=g_app.account_ui->navigation_view;auto& f=fixture;
    constexpr u32 first=kApplicationPluginPopupActionFirst;
    // Text in voice: the readable voice channel leads with its native Mark as read row.
    check(open(S::channel,89,2) && g_app.popup_scene.items[0].action==17 && first_plugin_row()==1 &&
        utf8_equal(g_app.popup_scene.items[1].label,utf8_literal("Open channel options")) &&
        (g_app.popup_scene.items[1].flags&popup_item_separator_before) &&
        !(g_app.popup_scene.items[2].flags&popup_item_enabled) && state.selected_channel_id==88 &&
        application_plugins_menu_action(first) && f.callbacks==1 && f.dismissed &&
        f.last.target==MC_MENU_CHANNEL && f.last.item_id==1 && f.last.channel_id==89 && f.last.guild_id==7 &&
        !f.last.user_id && !f.last.message_id && !f.last.flags && !g_app.popup_scene.active,
        "navigation runtime: channel menu keeps the disabled row and dispatches typed channel context after dismissal");
    auto callbacks=f.callbacks;
    check(open(S::channel,89,2) && application_plugins_menu_action(first+1) && f.callbacks==callbacks &&
        !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: a disabled row cannot enter the plugin");
    check(open(S::guild,7,1) && utf8_equal(g_app.popup_scene.items[0].label,utf8_literal("Copy server ID")) &&
        (g_app.popup_scene.items[1].flags&popup_item_danger) &&
        application_plugins_menu_action(first) && f.callbacks==callbacks+1 && f.last.target==MC_MENU_GUILD &&
        f.last.guild_id==7 && !f.last.channel_id && !f.last.message_id,
        "navigation runtime: danger row follows the native copy-ID row and dispatches guild identity");
    callbacks=f.callbacks;
    check(open(S::channel,88,2),"navigation runtime: readable native channel menu captures target");
    f.channel_rows[0].selectable=false;
    check(application_plugins_menu_action(first) && f.callbacks==callbacks && !g_app.popup_scene.active,
        "navigation runtime: removed selectable navigation target rejects activation despite unchanged model ID");
    f.channel_rows[0].selectable=true;
    check(open(S::channel,88,2),"navigation runtime: live row recaptures target");
    ++view.channel_revision;
    check(application_plugins_menu_action(first) && f.callbacks==callbacks,
        "navigation runtime: stale viewport source revision refuses captured plugin command");
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() && open(S::channel,88,2),
        "navigation runtime: navigation rebuild restores current source guard");
    auto* record=const_cast<ChannelRecord*>(chat_state_store_find_channel(state,88));record->effective_permissions=0;
    check(application_plugins_menu_action(first) && f.callbacks==callbacks,
        "navigation runtime: current-row menu cannot bypass changed channel view permission");
    record->effective_permissions=kPermissionViewChannel|kPermissionReadMessageHistory;
    check(open(S::guild,7,1),"navigation runtime: guild target recaptures");
    auto* guild=const_cast<GuildRecord*>(chat_state_store_find_guild(state,7));guild->unavailable=true;
    check(application_plugins_menu_action(first) && f.callbacks==callbacks,
        "navigation runtime: unavailable guild rejects captured server action");guild->unavailable=false;
    // A changed menu revision (the plugin unregistered or registered an item)
    // expires every captured token, even for an unrelated item.
    check(open(S::channel,88,2) && f.menus->unregister_item(f.plugin_binding,f.generation,3)==MC_OK &&
        application_plugins_menu_action(first) && f.callbacks==callbacks && !g_app.popup_scene.active &&
        open(S::channel,88,1),
        "navigation runtime: menu revision change rejects the captured row before plugin entry");
    check(open(S::channel,88,1),"navigation runtime: fresh scope captures readable target");
    const auto captured=application_plugins_menu_state().account_serial;const auto serial=g_app.plugin_account_scope_serial;
    application_plugins_account_scope_invalidated();
    check(g_app.plugin_account_scope_serial==serial+1 && !g_app.popup_scene.active &&
        !application_plugins_menu_state().count && !plugin_ui_scope_current(g_app.plugin_account_scope_serial,captured),
        "navigation runtime: account invalidator dismisses the borrowing popup, clears rows and expires the copied target");
    f.invalidate_in_callback=true;
    check(open(S::guild,7,1) && application_plugins_menu_action(first) && f.callbacks==callbacks+1 && f.dismissed &&
        !g_app.popup_scene.active,
        "navigation runtime: nested account invalidation inside the handler is tolerated after dismissal");
    f.invalidate_in_callback=false;
    callbacks=f.callbacks;
    // A 1:1 DM row: private popup owner, channel rows flagged private, then the
    // recipient's user rows.
    auto* direct=const_cast<ChannelRecord*>(chat_state_store_find_channel(state,88));
    const auto old_type=direct->type;const auto old_guild=direct->guild_id;
    direct->type=1;direct->guild_id=0;f.channel_rows[0].type=1;
    ++direct->revision;f.channel_rows[0].revision=direct->revision;++view.channel_revision;
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() &&
        open(S::channel,88,2) && g_app.popup_owner_kind==PopupOwnerKind::private_channel &&
        application_plugins_menu_state().bound &&
        utf8_equal(g_app.popup_scene.items[first_plugin_row()+1].label,utf8_literal("Inspect user")) &&
        application_plugins_menu_action(first) && f.callbacks==callbacks+1 && f.last.target==MC_MENU_CHANNEL &&
        f.last.channel_id==88 && f.last.user_id==kFriend && f.last.flags==MC_MENU_PRIVATE && f.dismissed &&
        open(S::channel,88,2) && application_plugins_menu_action(first+1) && f.callbacks==callbacks+2 &&
        f.last.target==MC_MENU_USER && f.last.user_id==kFriend && f.last.channel_id==88 && !f.last.flags,
        "navigation runtime: DM row binds to the private owner with private channel rows and recipient user rows");
    dismiss_popup();
    direct->type=old_type;direct->guild_id=old_guild;f.channel_rows[0].type=old_type;
    ++direct->revision;f.channel_rows[0].revision=direct->revision;++view.channel_revision;
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok(),
        "navigation runtime: channel source restores after isolated DM transition");
    // User menus (member list, authors, voice rows, profiles, friends rows).
    callbacks=f.callbacks;
    check(open(S::user,kSelf,1,true,7) && application_plugins_menu_action(first) && f.callbacks==callbacks+1 &&
        f.last.target==MC_MENU_USER && f.last.user_id==kSelf && f.last.guild_id==7 && f.last.flags==MC_MENU_OWN &&
        open(S::user,kFriend,1,true,7) && g_app.popup_scene.item_count==1 &&
        !(g_app.popup_scene.items[0].flags&popup_item_separator_before),
        "navigation runtime: user menu dispatches the user and server, flagging the own user");
    check(native_plugin_host_disable(application_settings_plugin_host_current(),owner+1).error!=NativePluginError::none && g_app.popup_scene.active,
        "navigation runtime: unrelated owner leaves the borrowing user menu open");
    record=const_cast<ChannelRecord*>(chat_state_store_find_channel(state,88));
    guild=const_cast<GuildRecord*>(chat_state_store_find_guild(state,7));
    ++guild->generation;
    check(application_plugins_menu_action(first) && f.callbacks==callbacks+1,
        "navigation runtime: a recreated server rejects a captured user-menu row");
    --guild->generation;
    check(open(S::guild,7,1,false),"navigation runtime: unbound guild bank captures for wrong-owner fence");
    application_popup_plugin_owner_transition(PopupOwnerKind::channel);
    g_app.popup_owner_kind=PopupOwnerKind::channel;
    check(!application_plugins_menu_bind() && !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: a guild bank cannot bind to a mismatched channel owner");
    const auto before_generation=f.callbacks;
    check(open(S::channel,88,1),"navigation runtime: owner transition binds generation fixture");
    ++g_app.popup_scene.widgets.generation;
    check(application_plugins_menu_action(first) &&
        f.callbacks==before_generation && !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: preserving captured rows still rejects a changed popup widget generation");
    constexpr PopupOwnerKind ordinary_owners[]{PopupOwnerKind::none,
        PopupOwnerKind::private_notice,PopupOwnerKind::autocomplete,PopupOwnerKind::commands,
        PopupOwnerKind::attachments,PopupOwnerKind::composer_edit,
        PopupOwnerKind::settings_choice,PopupOwnerKind::account_menu};
    const auto before_dismissal=f.callbacks;
    bool cleared=true;
    for (auto next:ordinary_owners) {
        cleared=cleared && open(S::guild,7,1,false);
        application_popup_plugin_owner_transition(next);g_app.popup_owner_kind=next;
        cleared=cleared && !application_plugins_menu_state().count &&
            application_plugins_menu_action(first) && f.callbacks==before_dismissal && !g_app.popup_scene.active;
    }
    check(cleared,"navigation runtime: every non-plugin popup transition clears rows and cannot activate old captured action");
    // Eight plugin rows at most per menu, across all items.
    bool registered=true;
    for(u64 id=10;id<18;++id)
        registered=registered && f.menus->register_item(f.plugin_binding,f.generation,id,MC_MENU_CHANNEL,{"Extra",5},0)==MC_OK;
    check(registered && open(S::channel,88,8) && application_plugins_menu_state().count==8,
        "navigation runtime: a native menu shows at most eight plugin rows");
    check(native_plugin_host_disable(application_settings_plugin_host_current(),owner).ok() && !g_app.popup_scene.active && !application_plugins_menu_state().count &&
        native_plugin_sdk_destroy(f.binding),
        "navigation runtime: owner detach closes the borrowing channel menu before removing its items");
    f.binding=nullptr;
    cleanup();
}
} // namespace maicord

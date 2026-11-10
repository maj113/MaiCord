#include "shell/state.hpp"
#include "../../../src/app/plugins/menus.cpp"
#include "../../support/ui_scene_fixtures.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../support/plugin_ui_fixture_runtime.hpp"
#include "settings/settings.hpp"
#include "plugins/plugins.hpp"
#include "plugins/surface_host.hpp"
#include "plugins/surfaces_internal.hpp"
#include "chat/messages/interaction.hpp"
#include "plugins/popup_plugin_owner.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/ui/guild/channel_context_policy.hpp"
#include "maicord/ui/guild/context_policy.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "shell/input.hpp"
#include "../../support/window_fixture.hpp"

namespace maicord::app_detail {
extern u32 channel_prefetch_calls; // settings_runtime_stubs.cpp: calls into the channel preload seam
const ApplicationPluginMenu& application_plugins_menu_state() noexcept { return g_menu; }
} // namespace maicord::app_detail

// Plugin rows in the native user, channel, DM and server menus, through the real plugin runtime (the UI
// fixture, an ABI-6 plugin), the shipping projection, binder and dispatcher, and the actual navigation scenes.
// The fixture's menus: Inspect and Others only (user), Wipe (channel and server; danger) and Broken (server;
// its handler fails).
namespace maicord {
namespace plugin_navigation_runtime {
using namespace app_detail;
using plugin_ui_fixture_runtime::State;
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
    bool account_owned;
} fixture{};
plugin_ui_fixture_runtime::Fixture plugin{};

State* state() noexcept { return plugin.state(); }
bool initialize() noexcept {
    auto& f = fixture;
    if (g_app.account_ui || g_app.settings) return false;
    g_app.account_ui=static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr,sizeof(AccountUiRuntime),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!g_app.account_ui)return false;
    f.account_owned=true;
    g_app.account.status.profile_id=77;g_app.login.visible=false;g_app.renderer_ready=true;g_app.renderer.dpi=96;
    g_app.context_focus=nullptr;g_app.text_form=nullptr;
    application_plugins_account_scope_invalidated();
    auto& chat=g_app.account_ui->chat_state;auto& view=g_app.account_ui->navigation_view;
    if(!chat_state_store_initialize(&chat,{f.guilds,2},{f.guild_order,2},{f.channels,4},{f.channel_order,4},{f.names,sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&chat,{7,utf8_literal("Server"),GuildMutationKind::create,1}).ok())return false;
    for(u32 index=0;index<2;++index) {
        ChannelInput input{};input.id=88+index;input.guild_id=7;input.type=index?2:0;
        input.name=utf8_literal("Navigation target");input.name_known=true;
        if(!chat_state_store_upsert_channel(&chat,input).ok())return false;
        auto* record=const_cast<ChannelRecord*>(chat_state_store_find_channel(chat,input.id));
        record->permissions_resolved=true;record->effective_permissions=kPermissionViewChannel|kPermissionReadMessageHistory;
        record->actions=channel_action_view|channel_action_read_history;
        f.channel_rows[index]={};f.channel_rows[index].id=input.id;f.channel_rows[index].type=input.type;f.channel_rows[index].selectable=true;
        f.channel_rows[index].revision=record->revision;f.channel_rows[index].generation=record->generation;
    }
    const u64 friend_id=kFriend;
    if(!private_channel_store_initialize(&f.private_channels,{f.private_records,2},{f.recipients,4}) ||
        !private_channel_store_replace(&f.private_channels,{88,0,{&friend_id,1},{},true}))return false;
    chat.private_channels=&f.private_channels;chat.self_user_id=kSelf;
    chat.selected_guild_id=7;chat.selected_channel_id=88;
    chat.roles=&f.role;chat.overwrites=&f.overwrite;
    view={};view.guild_rows=f.guild_rows;view.guild_capacity=view.guild_count=1;
    view.channel_rows=f.channel_rows;view.channel_capacity=view.channel_count=2;
    view.guild_revision=view.channel_revision=1;view.selected_guild_id=7;view.selected_channel_id=88;
    f.guild_rows[0]={};f.guild_rows[0].id=7;
    return guild_rail_scene_initialize(&g_app.guild_rail_scene,{f.rail_rows,2},{f.rail_widgets,4},{f.rail_nodes,4}) &&
        guild_rail_scene_update(&g_app.guild_rail_scene,&view,{0,48,72,700},0).ok() &&
        shell_navigation_scene_initialize(&g_app.shell_navigation_scene,{f.navigation_rows,2},{f.navigation_widgets,10},{f.navigation_nodes,10}) &&
        shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() &&
        popup_scene_initialize(&g_app.popup_scene,{f.popup_items,16},{f.popup_rows,16},{f.popup_widgets,34},{f.popup_nodes,34}) &&
        plugin.enable() && state();
}
// Native rows, then the projected plugin rows, then the shipping owner
// transition and binder, as the real menu builders do.
bool open(ApplicationPluginMenuSurface surface,u64 id,u32 expected,bool bind=true,u64 guild=0) noexcept {
    auto& chat=g_app.account_ui->chat_state;
    PopupItem items[12]{};
    const u32 builtins=surface==ApplicationPluginMenuSurface::guild?guild_context_policy_build(chat,id,19,{items,1}):
        surface==ApplicationPluginMenuSurface::channel?channel_context_policy_build(chat,id,17,18,{items,2}):0;
    const u32 extra=application_plugins_menu_items(surface,id,{items+builtins,8},guild);
    if(extra!=expected)return false;
    if(!builtins)items[0].flags &= ~popup_item_separator_before;
    if (!popup_scene_open(&g_app.popup_scene,PopupKind::context_menu,id,{items,builtins+extra},
        {40,60,41,61},{0,0,1000,700},240).ok()) return false;
    const auto* channel=surface==ApplicationPluginMenuSurface::channel?chat_state_store_find_channel(chat,id):nullptr;
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
bool label_is(u32 row,const char* text) noexcept {
    return row<g_app.popup_scene.item_count && utf8_equal(g_app.popup_scene.items[row].label,{text,c_string_length(text)});
}
void cleanup() noexcept {
    if(!fixture.account_owned)return;
    dismiss_popup();
    plugin.release();
    application_settings_release();popup_scene_release(&g_app.popup_scene);g_app.popup_scene={};
    g_app.guild_rail_scene={};g_app.shell_navigation_scene={};g_app.context_focus=nullptr;
    if(g_app.account_ui)VirtualFree(g_app.account_ui,0,MEM_RELEASE);g_app.account_ui=nullptr;fixture.account_owned=false;
}
} // namespace plugin_navigation_runtime
void test_application_plugin_navigation_runtime(void (*check)(bool,const char*) noexcept) noexcept {
    using namespace plugin_navigation_runtime;using namespace app_detail;using S=ApplicationPluginMenuSurface;
    const bool initialized=initialize();check(initialized,"navigation runtime: plugin, navigation scenes and popup initialize");
    if(!initialized){cleanup();return;}
    auto& chat=g_app.account_ui->chat_state;auto& view=g_app.account_ui->navigation_view;auto& f=fixture;
    constexpr u32 first=kApplicationPluginPopupActionFirst;
    State* s=state();
    // Text in voice: the readable voice channel leads with its native Mark as read row, then the plugin's danger row.
    check(open(S::channel,89,1) && g_app.popup_scene.items[0].action==17 && first_plugin_row()==1 && label_is(1,"Wipe") &&
        (g_app.popup_scene.items[1].flags&popup_item_separator_before) && (g_app.popup_scene.items[1].flags&popup_item_danger) &&
        chat.selected_channel_id==88 && application_plugins_menu_action(first) && s->menu_calls==1 && s->menu_gesture &&
        s->menu_kind==MC_MENU_CHANNEL && s->menu_item==4 && s->channel_id==89 && s->guild_id==7 &&
        !s->user_id && !s->message_id && !s->menu_flags && !g_app.popup_scene.active,
        "navigation runtime: channel menu follows the native row with a danger row and dispatches typed channel context after dismissal");
    auto calls=s->menu_calls;
    check(open(S::guild,7,2) && label_is(0,"Copy server ID") && label_is(1,"Wipe") && label_is(2,"Broken") &&
        (g_app.popup_scene.items[1].flags&popup_item_danger) && !(g_app.popup_scene.items[2].flags&popup_item_danger) &&
        application_plugins_menu_action(first) && s->menu_calls==calls+1 && s->menu_kind==MC_MENU_GUILD &&
        s->guild_id==7 && !s->channel_id && !s->message_id,
        "navigation runtime: danger row follows the native copy-ID row and dispatches guild identity");
    calls=s->menu_calls;
    check(open(S::guild,7,2) && application_plugins_menu_action(first+1) && s->menu_calls==calls+1 && s->menu_item==8 &&
        !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: a handler that fails leaves no popup and no captured rows");
    calls=s->menu_calls;
    check(open(S::channel,88,1),"navigation runtime: readable native channel menu captures target");
    f.channel_rows[0].selectable=false;
    check(application_plugins_menu_action(first) && s->menu_calls==calls && !g_app.popup_scene.active,
        "navigation runtime: removed selectable navigation target rejects activation despite unchanged model ID");
    f.channel_rows[0].selectable=true;
    check(open(S::channel,88,1),"navigation runtime: live row recaptures target");
    ++view.channel_revision;
    check(application_plugins_menu_action(first) && s->menu_calls==calls,
        "navigation runtime: stale viewport source revision refuses captured plugin command");
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() && open(S::channel,88,1),
        "navigation runtime: navigation rebuild restores current source guard");
    auto* record=const_cast<ChannelRecord*>(chat_state_store_find_channel(chat,88));record->effective_permissions=0;
    check(application_plugins_menu_action(first) && s->menu_calls==calls,
        "navigation runtime: current-row menu cannot bypass changed channel view permission");
    record->effective_permissions=kPermissionViewChannel|kPermissionReadMessageHistory;
    check(open(S::guild,7,2),"navigation runtime: guild target recaptures");
    auto* guild=const_cast<GuildRecord*>(chat_state_store_find_guild(chat,7));guild->unavailable=true;
    check(application_plugins_menu_action(first) && s->menu_calls==calls,
        "navigation runtime: unavailable guild rejects captured server action");guild->unavailable=false;
    // Enabling or disabling any plugin moves the registry revision and expires every captured token.
    const u32 revision=plugin_ui_revision();
    check(open(S::channel,88,1) && plugin.disable() && plugin_ui_revision()!=revision &&
        application_plugins_menu_action(first) && !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: a disabled plugin rejects the captured row before it could enter freed code");
    PopupItem none[8]{};
    check(!application_plugins_menu_items(S::channel,88,{none,8}) && !application_plugins_menu_state().count,
        "navigation runtime: a disabled plugin projects no rows");
    check(plugin.restart() && (s=state()) && open(S::channel,88,1),"navigation runtime: a new enable projects its rows again");
    calls=s->menu_calls;
    const auto captured=application_plugins_menu_state().account_serial;const auto serial=g_app.plugin_account_scope_serial;
    application_plugins_account_scope_invalidated();
    check(g_app.plugin_account_scope_serial==serial+1 && !g_app.popup_scene.active &&
        !application_plugins_menu_state().count && !plugin_ui_scope_current(g_app.plugin_account_scope_serial,captured),
        "navigation runtime: account invalidator dismisses the borrowing popup, clears rows and expires the copied target");
    // A 1:1 DM row: private popup owner, channel rows flagged private, then the recipient's user rows.
    auto* direct=const_cast<ChannelRecord*>(chat_state_store_find_channel(chat,88));
    const auto old_type=direct->type;const auto old_guild=direct->guild_id;
    direct->type=1;direct->guild_id=0;f.channel_rows[0].type=1;
    ++direct->revision;f.channel_rows[0].revision=direct->revision;++view.channel_revision;
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok() &&
        open(S::channel,88,3) && g_app.popup_owner_kind==PopupOwnerKind::private_channel &&
        application_plugins_menu_state().bound && label_is(first_plugin_row()+1,"Inspect") &&
        label_is(first_plugin_row()+2,"Others only") &&
        application_plugins_menu_action(first) && s->menu_calls==calls+1 && s->menu_kind==MC_MENU_CHANNEL &&
        s->channel_id==88 && s->user_id==kFriend && s->menu_flags==MC_MENU_PRIVATE &&
        open(S::channel,88,3) && application_plugins_menu_action(first+1) && s->menu_calls==calls+2 &&
        s->menu_kind==MC_MENU_USER && s->user_id==kFriend && s->channel_id==88 && !s->menu_flags,
        "navigation runtime: DM row binds to the private owner with private channel rows and recipient user rows");
    dismiss_popup();
    direct->type=old_type;direct->guild_id=old_guild;f.channel_rows[0].type=old_type;
    ++direct->revision;f.channel_rows[0].revision=direct->revision;++view.channel_revision;
    check(shell_navigation_scene_update(&g_app.shell_navigation_scene,&view,{72,48,312,700},0).ok(),
        "navigation runtime: channel source restores after isolated DM transition");
    // User menus (member list, authors, voice rows, profiles, friends rows): your own account hides "Others only".
    calls=s->menu_calls;
    check(open(S::user,kSelf,1,true,7) && label_is(0,"Inspect") && application_plugins_menu_action(first) &&
        s->menu_calls==calls+1 && s->menu_kind==MC_MENU_USER && s->user_id==kSelf && s->guild_id==7 &&
        s->menu_flags==MC_MENU_OWN && open(S::user,kFriend,2,true,7) && g_app.popup_scene.item_count==2 &&
        !(g_app.popup_scene.items[0].flags&popup_item_separator_before) && label_is(1,"Others only"),
        "navigation runtime: user menu dispatches the user and server, flagging the own user and hiding rows meant for others");
    dismiss_popup();
    guild=const_cast<GuildRecord*>(chat_state_store_find_guild(chat,7));
    check(open(S::user,kFriend,2,true,7),"navigation runtime: user menu recaptures its server");
    ++guild->generation;
    check(application_plugins_menu_action(first) && s->menu_calls==calls+1,
        "navigation runtime: a recreated server rejects a captured user-menu row");
    --guild->generation;
    check(open(S::guild,7,2,false),"navigation runtime: unbound guild bank captures for wrong-owner fence");
    application_popup_plugin_owner_transition(PopupOwnerKind::channel);
    g_app.popup_owner_kind=PopupOwnerKind::channel;
    check(!application_plugins_menu_bind() && !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: a guild bank cannot bind to a mismatched channel owner");
    const auto before_generation=s->menu_calls;
    check(open(S::channel,88,1),"navigation runtime: owner transition binds generation fixture");
    ++g_app.popup_scene.widgets.generation;
    check(application_plugins_menu_action(first) &&
        s->menu_calls==before_generation && !g_app.popup_scene.active && !application_plugins_menu_state().count,
        "navigation runtime: preserving captured rows still rejects a changed popup widget generation");
    constexpr PopupOwnerKind ordinary_owners[]{PopupOwnerKind::none,
        PopupOwnerKind::private_notice,PopupOwnerKind::autocomplete,PopupOwnerKind::commands,
        PopupOwnerKind::attachments,PopupOwnerKind::composer_edit,
        PopupOwnerKind::settings_choice,PopupOwnerKind::account_menu};
    const auto before_dismissal=s->menu_calls;
    bool cleared=true;
    for (auto next:ordinary_owners) {
        cleared=cleared && open(S::guild,7,2,false);
        application_popup_plugin_owner_transition(next);g_app.popup_owner_kind=next;
        cleared=cleared && !application_plugins_menu_state().count &&
            application_plugins_menu_action(first) && s->menu_calls==before_dismissal && !g_app.popup_scene.active;
    }
    check(cleared,"navigation runtime: every non-plugin popup transition clears rows and cannot activate old captured action");
    // Hovering the list never starts a history preload; only a press that waits for its release does.
    {
        auto& scene=g_app.shell_navigation_scene;
        const bool rendering=g_app.renderer_ready;
        g_app.renderer_ready=false; // the fixture has no native text to rebuild on a hover change
        const u32 presses=channel_prefetch_calls;
        bool swept=scene.count==2;
        u64 last=0;
        for (u32 pass=0;swept && pass<3;++pass)
            for (u32 index=0;swept && index<scene.count;++index) {
                const auto* layout=shell_navigation_scene_layout_node(scene,scene.rows[index]);
                swept=layout && handle_navigation_hover(window_test::pointer_move(
                    (layout->rectangle.left+layout->rectangle.right)*.5f,(layout->rectangle.top+layout->rectangle.bottom)*.5f));
                last=scene.rows[index].view.id;
            }
        g_app.renderer_ready=rendering;
        const auto* hovered=shell_navigation_scene_resolve(scene,scene.widgets.hovered);
        check(swept && hovered && hovered->view.id==last && channel_prefetch_calls==presses,
            "navigation runtime: sweeping the pointer over channel rows hovers each and starts no preload");
    }
    cleanup();
}
// What the application rebuilds its text from when plugins decorate: a decorating plugin changes it, and so does
// its going away (its decorations leave the text on the next build, though no plugin is installed any more).
void test_application_plugin_surfaces_revision(void (*check)(bool,const char*) noexcept) noexcept {
    using namespace app_detail;
    static plugin_ui_fixture_runtime::Fixture decorating{};
    check(!g_plugin_surfaces_active && !application_plugin_surfaces_stale(),
        "plugin surfaces: with no plugin nothing is stale");
    check(decorating.enable(L"MaiCordPluginSurfacesFixture.dll") && g_plugin_surfaces_active &&
        application_plugin_surfaces_stale(),
        "plugin surfaces: enabling a decorating plugin makes the built text stale");
    application_plugin_surfaces_rendered();
    check(!application_plugin_surfaces_stale(), "plugin surfaces: building the text catches up");
    check(decorating.disable() && !g_plugin_surfaces_active && application_plugin_surfaces_stale(),
        "plugin surfaces: a plugin that left makes the text stale though no plugin is installed");
    application_plugin_surfaces_rendered();
    check(!application_plugin_surfaces_stale(), "plugin surfaces: the rebuild after it left catches up");
    decorating.release();
}
// The cache reader plugins read through (surface_host.cpp) over a small account: the group DMs shared with a
// user, most recently active first; a voice channel found in any server; a server's overview counts.
void test_application_plugin_reads(void (*check)(bool,const char*) noexcept) noexcept {
    using namespace app_detail;
    struct Reads {
        GuildRecord guilds[1]; ChannelRecord channels[4]; u16 guild_order[1], channel_order[4]; u8 names[512];
        PrivateChannelStore private_channels; PrivateChannelRecord private_records[3]; u64 recipients[6];
    };
    static Reads r{};
    if (g_app.account_ui) { check(false, "plugin reads: no account is open"); return; }
    g_app.account_ui=static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr,sizeof(AccountUiRuntime),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!g_app.account_ui) { check(false, "plugin reads: account allocates"); return; }
    auto& ui=*g_app.account_ui;auto& chat=ui.chat_state;
    bool ready=chat_state_store_initialize(&chat,{r.guilds,1},{r.guild_order,1},{r.channels,4},{r.channel_order,4},{r.names,sizeof(r.names)}) &&
        chat_state_store_upsert_guild(&chat,{7,utf8_literal("Server"),GuildMutationKind::create,1}).ok() &&
        private_channel_store_initialize(&r.private_channels,{r.private_records,3},{r.recipients,6}) &&
        voice_state_store_initialize_owned(&ui.voice_states,16);
    // Group DMs 200 and 201 include user 501 (201 is the more recent), 202 doesn't; 89 is a voice channel of server 7.
    const u64 both[2]={501,502}, other[1]={502};
    for (u32 i=0;ready && i<4;++i) {
        ChannelInput input{};input.id=i<3?200+i:89;input.guild_id=i<3?0:7;input.type=i<3?3:2;
        input.last_message_id=i==1?(5000ull<<22):(1000ull<<22);input.last_message_known=true;
        ready=chat_state_store_upsert_channel(&chat,input).ok() &&
            (i==3 || private_channel_store_replace(&r.private_channels,{200+i,0,i<2?ConstSlice<u64>{both,2}:ConstSlice<u64>{other,1},{},true}));
    }
    chat.private_channels=&r.private_channels;chat.self_user_id=plugin_navigation_runtime::kSelf;
    ready=ready && voice_state_store_apply(&ui.voice_states,7,89,501,0)==VoiceStateResult::changed;
    check(ready,"plugin reads: the account is set up");
    u64 groups[1]={};
    u32 total=0;
    check(plugin_surfaces_detail::read_shared_group_dms(501,groups,1,&total)==MC_OK && total==2 && groups[0]==201,
        "plugin reads: shared group DMs count them all and list the most recent first");
    check(plugin_surfaces_detail::read_shared_group_dms(503,groups,1,&total)==MC_E_NOT_FOUND && !total,
        "plugin reads: a user in no group DM shares none");
    u64 voice=0;
    check(plugin_surfaces_detail::read_voice_channel(501,0,&voice)==MC_OK && voice==89 &&
        plugin_surfaces_detail::read_voice_channel(501,8,&voice)==MC_E_NOT_FOUND && !voice,
        "plugin reads: a voice channel is found in any server, not in another");
    mc_guild guild{};
    check(plugin_surfaces_detail::read_guild(7,&guild)==MC_OK && guild.emoji_count==0 && guild.boost_count==0,
        "plugin reads: a server's overview counts what the cache holds");
    voice_state_store_release(&ui.voice_states);
    VirtualFree(g_app.account_ui,0,MEM_RELEASE);g_app.account_ui=nullptr;
}
} // namespace maicord

#include "probe_present.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
// This is fixture geometry, not an ApplicationSettings owner. Descriptors are
// copied from src/app/application_settings.cpp project()/ensure() because the
// standalone UiProbe does not link the application's settings/account owners.
// Rendering, page registry, widgets, layout, text, clipping and focus use the
// exact production UiSettingsScene APIs. No host action is dispatched.
struct Fixture {
    UiPage pages[5];
    UiSettingsPageContent contents[5];
    UiSettingsControl controls[6];
    UiSettingsNavEntry navigation[6];
    UiPageRegistry registry;
    UiSettingsScene scene;
};
Fixture fixture;
constexpr Utf8View volume_choices[] = {utf8_literal("0%"),utf8_literal("1%"),utf8_literal("2%"),utf8_literal("3%"),utf8_literal("4%"),utf8_literal("5%"),utf8_literal("6%"),utf8_literal("7%"),utf8_literal("8%"),utf8_literal("9%"),utf8_literal("10%"),utf8_literal("11%"),utf8_literal("12%"),utf8_literal("13%"),utf8_literal("14%"),utf8_literal("15%"),utf8_literal("16%"),utf8_literal("17%"),utf8_literal("18%"),utf8_literal("19%"),utf8_literal("20%"),utf8_literal("21%"),utf8_literal("22%"),utf8_literal("23%"),utf8_literal("24%"),utf8_literal("25%"),utf8_literal("26%"),utf8_literal("27%"),utf8_literal("28%"),utf8_literal("29%"),utf8_literal("30%"),utf8_literal("31%"),utf8_literal("32%"),utf8_literal("33%"),utf8_literal("34%"),utf8_literal("35%"),utf8_literal("36%"),utf8_literal("37%"),utf8_literal("38%"),utf8_literal("39%"),utf8_literal("40%"),utf8_literal("41%"),utf8_literal("42%"),utf8_literal("43%"),utf8_literal("44%"),utf8_literal("45%"),utf8_literal("46%"),utf8_literal("47%"),utf8_literal("48%"),utf8_literal("49%"),utf8_literal("50%"),utf8_literal("51%"),utf8_literal("52%"),utf8_literal("53%"),utf8_literal("54%"),utf8_literal("55%"),utf8_literal("56%"),utf8_literal("57%"),utf8_literal("58%"),utf8_literal("59%"),utf8_literal("60%"),utf8_literal("61%"),utf8_literal("62%"),utf8_literal("63%"),utf8_literal("64%"),utf8_literal("65%"),utf8_literal("66%"),utf8_literal("67%"),utf8_literal("68%"),utf8_literal("69%"),utf8_literal("70%"),utf8_literal("71%"),utf8_literal("72%"),utf8_literal("73%"),utf8_literal("74%"),utf8_literal("75%"),utf8_literal("76%"),utf8_literal("77%"),utf8_literal("78%"),utf8_literal("79%"),utf8_literal("80%"),utf8_literal("81%"),utf8_literal("82%"),utf8_literal("83%"),utf8_literal("84%"),utf8_literal("85%"),utf8_literal("86%"),utf8_literal("87%"),utf8_literal("88%"),utf8_literal("89%"),utf8_literal("90%"),utf8_literal("91%"),utf8_literal("92%"),utf8_literal("93%"),utf8_literal("94%"),utf8_literal("95%"),utf8_literal("96%"),utf8_literal("97%"),utf8_literal("98%"),utf8_literal("99%"),utf8_literal("100%"),utf8_literal("101%"),utf8_literal("102%"),utf8_literal("103%"),utf8_literal("104%"),utf8_literal("105%"),utf8_literal("106%"),utf8_literal("107%"),utf8_literal("108%"),utf8_literal("109%"),utf8_literal("110%"),utf8_literal("111%"),utf8_literal("112%"),utf8_literal("113%"),utf8_literal("114%"),utf8_literal("115%"),utf8_literal("116%"),utf8_literal("117%"),utf8_literal("118%"),utf8_literal("119%"),utf8_literal("120%"),utf8_literal("121%"),utf8_literal("122%"),utf8_literal("123%"),utf8_literal("124%"),utf8_literal("125%"),utf8_literal("126%"),utf8_literal("127%"),utf8_literal("128%"),utf8_literal("129%"),utf8_literal("130%"),utf8_literal("131%"),utf8_literal("132%"),utf8_literal("133%"),utf8_literal("134%"),utf8_literal("135%"),utf8_literal("136%"),utf8_literal("137%"),utf8_literal("138%"),utf8_literal("139%"),utf8_literal("140%"),utf8_literal("141%"),utf8_literal("142%"),utf8_literal("143%"),utf8_literal("144%"),utf8_literal("145%"),utf8_literal("146%"),utf8_literal("147%"),utf8_literal("148%"),utf8_literal("149%"),utf8_literal("150%"),utf8_literal("151%"),utf8_literal("152%"),utf8_literal("153%"),utf8_literal("154%"),utf8_literal("155%"),utf8_literal("156%"),utf8_literal("157%"),utf8_literal("158%"),utf8_literal("159%"),utf8_literal("160%"),utf8_literal("161%"),utf8_literal("162%"),utf8_literal("163%"),utf8_literal("164%"),utf8_literal("165%"),utf8_literal("166%"),utf8_literal("167%"),utf8_literal("168%"),utf8_literal("169%"),utf8_literal("170%"),utf8_literal("171%"),utf8_literal("172%"),utf8_literal("173%"),utf8_literal("174%"),utf8_literal("175%"),utf8_literal("176%"),utf8_literal("177%"),utf8_literal("178%"),utf8_literal("179%"),utf8_literal("180%"),utf8_literal("181%"),utf8_literal("182%"),utf8_literal("183%"),utf8_literal("184%"),utf8_literal("185%"),utf8_literal("186%"),utf8_literal("187%"),utf8_literal("188%"),utf8_literal("189%"),utf8_literal("190%"),utf8_literal("191%"),utf8_literal("192%"),utf8_literal("193%"),utf8_literal("194%"),utf8_literal("195%"),utf8_literal("196%"),utf8_literal("197%"),utf8_literal("198%"),utf8_literal("199%"),utf8_literal("200%")};
constexpr UiPageKey notification_page{1,5};
bool visible_text(const UiTextCommandStream& text,Utf8View expected,UiRect viewport) noexcept {
    for(u32 i=0;i<text.count;++i){const auto& c=text.storage[i];if(!utf8_equal(c.text,expected))continue;
        if(c.owner.manager!=&fixture.scene.widgets||!ui_rect_has_area(c.rectangle)||!ui_rect_has_area(c.clip)||
            !ui_rect_equal(c.clip,ui_rect_intersection(c.clip,viewport)))return false;
        return ui_rect_has_area(ui_rect_intersection(c.rectangle,c.clip));
    }return false;
}
bool visible_page(const UiTextCommandStream& text,u32 index) noexcept {
    const auto expected=UiSettingsTarget{UiSettingsTargetKind::page,{1,index+1},0};
    for(u32 i=0;i<text.count;++i){const auto& c=text.storage[i];if(!utf8_equal(c.text,fixture.pages[index].label)||!ui_rect_has_area(c.clip))continue;
        if(ui_settings_target_equal(ui_settings_scene_hit_test(fixture.scene,(c.clip.left+c.clip.right)*.5f,(c.clip.top+c.clip.bottom)*.5f),expected))return true;
    }return false;
}
bool geometry(Fixture& f) noexcept {
    if(!ui_page_key_equal(f.scene.selected_page,notification_page)||f.scene.content.controls.count!=6)return false;
    UiTextCommand commands[64]{};UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    if(!ui_text_command_stream_initialize(&text,{commands,64})||!ui_settings_scene_text_output(f.scene,kDefaultTheme,&text_output))return false;
    if(!visible_text(text,utf8_literal("USER SETTINGS"),f.scene.viewport)||!visible_text(text,f.contents[4].title,f.scene.viewport)||
        (f.contents[4].description.bytes&&!visible_text(text,f.contents[4].description,f.scene.viewport)))return false;
    for(u32 i=0;i<5;++i)if(!visible_text(text,f.pages[i].label,f.scene.viewport)||!visible_page(text,i))return false;
    for(u32 i=0;i<6;++i){
        const UiSettingsTarget target{UiSettingsTargetKind::control,notification_page,i+1};
        const auto* descriptor=ui_settings_scene_control(f.scene,target);UiRect anchor{};
        if(!descriptor||descriptor->kind!=f.controls[i].kind||!utf8_equal(descriptor->label,f.controls[i].label)||
            !ui_settings_scene_control_anchor(f.scene,target,&anchor)||!ui_rect_has_area(anchor)||
            !ui_rect_equal(anchor,ui_rect_intersection(anchor,f.scene.viewport))||
            !ui_settings_target_equal(ui_settings_scene_hit_test(f.scene,(anchor.left+anchor.right)*.5f,(anchor.top+anchor.bottom)*.5f),target)||
            !visible_text(text,descriptor->label,f.scene.viewport)||(descriptor->description.bytes&&!visible_text(text,descriptor->description,f.scene.viewport)))return false;
        if(descriptor->kind==UiSettingsControlKind::action&&!visible_text(text,descriptor->action_label,f.scene.viewport))return false;
        UiSettingsActivation request{};
        if(!ui_settings_scene_activation(f.scene,target,false,&request)||request.kind!=descriptor->kind||
            !ui_settings_target_equal(request.target,target))return false;
        if(descriptor->kind==UiSettingsControlKind::toggle&&(request.checked==descriptor->checked))return false;
        if(i==5 && descriptor->kind==UiSettingsControlKind::choice&&
            (descriptor->choices.count!=201||descriptor->selected_choice!=100||request.selected_choice!=101||
             !utf8_equal(descriptor->choices[0],utf8_literal("0%"))||
             !utf8_equal(descriptor->choices[200],utf8_literal("200%"))||
             !visible_text(text,utf8_literal("100%"),f.scene.viewport)))return false;

        log_u64(LogLevel::info,"probe","notification_setting_id",i+1);
        log_u64(LogLevel::info,"probe","notification_setting_value_left_milli",static_cast<u64>(anchor.left*1000));
        log_u64(LogLevel::info,"probe","notification_setting_value_top_milli",static_cast<u64>(anchor.top*1000));
        log_u64(LogLevel::info,"probe","notification_setting_value_right_milli",static_cast<u64>(anchor.right*1000));
        log_u64(LogLevel::info,"probe","notification_setting_value_bottom_milli",static_cast<u64>(anchor.bottom*1000));
    }
    // Asking for semantic activations never changes the borrowed descriptors.
    return f.controls[0].checked&&f.controls[1].checked&&!f.controls[4].checked&&
        f.controls[5].selected_choice==100&&f.controls[5].choices.count==201;
}
}
bool notification_settings_ui_probe(D3D11Renderer& renderer,bool(*capture)(const wchar_t*) noexcept) noexcept {
    if(!capture||renderer.login_visible||!renderer.dpi||(renderer.popup_scene&&renderer.popup_scene->active))return false;
    auto& f=fixture;
    f.controls[0]={1,utf8_literal("Desktop notifications"),
        utf8_literal("Show notifications when MaiCord is in the background."),UiSettingsControlKind::toggle,true,true,{},0,{}};
    f.controls[1]={2,utf8_literal("Message sounds"),
        utf8_literal("Play a sound for new messages."),UiSettingsControlKind::toggle,true,true,{},0,{}};
    f.controls[2]={3,utf8_literal("Set up desktop notifications"),
        utf8_literal("Desktop notifications aren't set up yet."),UiSettingsControlKind::action,true,false,{},0,utf8_literal("Set up")};
    f.controls[3]={4,utf8_literal("Windows notification settings"),
        utf8_literal("Manage notification permissions in Windows."),UiSettingsControlKind::action,true,false,{},0,utf8_literal("Open settings")};
    f.controls[4]={5,utf8_literal("Selected channel sounds"),
        utf8_literal("Play sounds for the channel you're viewing."),UiSettingsControlKind::toggle,true,false,{},0,{}};
    f.controls[5]={6,utf8_literal("Output volume"),
        Utf8View{},UiSettingsControlKind::choice,true,false,{volume_choices,201},100,{}};
    constexpr Utf8View labels[]={utf8_literal("Appearance"),utf8_literal("Accessibility"),utf8_literal("MaiCord"),utf8_literal("Plugins"),utf8_literal("Notifications")};
    constexpr Utf8View descriptions[]={utf8_literal("Theme, colors and layout."),utf8_literal("Control motion and scrolling."),Utf8View{},
        utf8_literal("Add features to MaiCord."),Utf8View{}};
    const UiSettingsScene* old_scene=renderer.settings_scene;const u32 width=renderer.width,height=renderer.height;
    struct Restore {
        D3D11Renderer& renderer;Fixture& f;const UiSettingsScene* old_scene;u32 width,height;
        ~Restore() noexcept {renderer_bind_settings_scene(&renderer,old_scene);ui_settings_scene_release(&f.scene);
            ui_page_registry_unregister_owner(&f.registry,1);ui_page_registry_clear(&f.registry);renderer_resize(&renderer,width,height);}
    } restore{renderer,f,old_scene,width,height};
    u32 stage=__LINE__;bool valid=ui_page_registry_initialize(&f.registry,{f.pages,5});
    for(u32 i=0;valid&&i<5;++i){f.contents[i]={labels[i],descriptions[i],i==4?ConstSlice<UiSettingsControl>{f.controls,6}:ConstSlice<UiSettingsControl>{},1};
        valid=ui_page_registry_register(&f.registry,{{1,i+1},labels[i],&f.contents[i]}).ok();}
    // The scene projects its sidebar from the host's navigation entries.
    f.navigation[0]={UiSettingsNavKind::header,false,UiIcon::count,utf8_literal("USER SETTINGS"),{},0};
    for(u32 i=0;i<5;++i)f.navigation[i+1]={UiSettingsNavKind::page,false,UiIcon::user,labels[i],{1,i+1},0};
    stage=__LINE__;
    valid=valid&&renderer_resize(&renderer,1200,970)&&ui_page_registry_select(&f.registry,notification_page).ok();
    const float scale=static_cast<float>(renderer.dpi)/96.f;const UiRect viewport{0,0,1200/scale,970/scale};
    valid=valid&&ui_settings_scene_bind_navigation(&f.scene,{f.navigation,6},1)&&ui_settings_scene_update(&f.scene,&f.registry,viewport)&&
        ui_settings_scene_focus(&f.scene,{UiSettingsTargetKind::page,notification_page,0})&&geometry(f);
    if(valid){renderer_bind_settings_scene(&renderer,&f.scene);ui_settings_scene_set_focus_visible(&f.scene,false);
        stage=__LINE__;valid=renderer_rebuild_content_text(&renderer,kDefaultTheme)&&probe_present(&renderer,kDefaultTheme)&&
            renderer.interaction_text_settings_revision==f.scene.revision;}
    if(valid){const u32 builds=renderer.paint_stream_builds;valid=probe_present(&renderer,kDefaultTheme)&&renderer.paint_stream_builds==builds;}
    if(valid){stage=__LINE__;const u32 builds=renderer.paint_stream_builds;
        valid=ui_settings_scene_move_focus(&f.scene,false)&&ui_settings_scene_move_focus(&f.scene,false)&&ui_settings_scene_move_focus(&f.scene,false)&&
            ui_settings_target_equal(ui_settings_scene_focused(f.scene),{UiSettingsTargetKind::control,notification_page,3});
        ui_settings_scene_set_focus_visible(&f.scene,true);
        valid=valid&&renderer_rebuild_content_text(&renderer,kDefaultTheme)&&probe_present(&renderer,kDefaultTheme)&&
            renderer.paint_stream_builds>builds&&renderer.interaction_text_settings_revision==f.scene.revision&&capture(L"notification-settings-keyboard.bmp");}
    if(!valid)log_u64(LogLevel::error,"probe","notification_settings_failure_line",stage);
    log_u64(LogLevel::info,"probe","notification_settings_fixture_dpi",renderer.dpi);
    log_u64(LogLevel::info,"probe","notification_settings_fixture_width",renderer.width);
    log_u64(LogLevel::info,"probe","notification_settings_fixture_height",renderer.height);
    log_u64(LogLevel::info,"probe","notification_settings_fixture_capture_count",valid?1u:0u);
    log_write(valid?LogLevel::info:LogLevel::error,"probe",valid?"notification_settings_copied_production_descriptors_native_fixture_geometry_passed":"notification_settings_native_fixture_failed");
    return valid;
}
}

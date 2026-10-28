#include "maicord/ui/text_form.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "fake_text_measure.hpp"

namespace maicord {
namespace {
struct ConfirmationMeasure {u32 calls=0;};
bool confirmation_measure(void* context,const UiTextMeasureRequest& request,UiTextMeasureValue* value) noexcept {
    ++static_cast<ConfirmationMeasure*>(context)->calls;
    return fake_text_measure<true>(nullptr,request,value);
}
}
void test_text_form_confirmation(void (*check)(bool,const char*) noexcept) noexcept {
    TextFormScene scene{};ConfirmationMeasure measured{};UiTextMeasureRecord records[32]{};UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache,{records,32},confirmation_measure,&measured),"confirmation_native_measure_initialize");
    TextFormContent content{};content.title=utf8_literal("Remove friend?");
    content.context=utf8_literal("Removing this friend changes your relationship. You can cancel and return to your Friends list. "
        "The account owns this decision and sends the request only after you choose Remove. This explanation wraps inside the native dialog.");
    content.status=utf8_literal("This action needs your confirmation.");content.cancel=utf8_literal("Cancel");content.accept=utf8_literal("Remove");
    content.can_accept=true;content.focus_visible=true;content.confirmation_only=true;
    const UiRect viewport{0,0,320,400};
    check(text_form_update(&scene,content,viewport,&cache)&&scene.active&&scene.widgets.count==kTextFormElements-3-kTextFormChoiceElements&&!text_form_node(scene,TextFormElement::option)&&
        !text_form_node(scene,TextFormElement::edit)&&!text_form_node(scene,TextFormElement::label)&&
        text_form_focused(scene)==TextFormElement::cancel,"confirmation_has_no_edit_or_label_and_initial_cancel_focus");
    if(!scene.active){text_form_release(&scene);return;}
    UiLayoutDiagnostic diagnostics[32]{};
    check(ui_layout_engine_validate(scene.widgets.layout,{diagnostics,32}).clean()&&
        ui_rect_height(text_form_node(scene,TextFormElement::context)->content_rectangle)>40,
        "confirmation_context_wraps_in_retained_native_layout");
    check(!text_form_focus(&scene,TextFormElement::edit)&&text_form_focused(scene)==TextFormElement::cancel&&
        text_form_move_focus(&scene,false)&&text_form_focused(scene)==TextFormElement::accept&&
        text_form_move_focus(&scene,false)&&text_form_focused(scene)==TextFormElement::cancel&&
        text_form_move_focus(&scene,true)&&text_form_focused(scene)==TextFormElement::accept,
        "confirmation_tab_wraps_only_buttons_and_missing_edit_cannot_clear_focus");
    const auto generation=scene.widgets.generation;const auto allocation=scene.storage.arena.base;
    const auto layout=scene.widgets.layout.revision;const auto calls=measured.calls;
    check(text_form_update(&scene,content,viewport,&cache)&&scene.widgets.generation==generation&&
        scene.widgets.layout.revision==layout&&scene.storage.arena.base==allocation&&measured.calls==calls,
        "confirmation_unchanged_update_reuses_native_layout_and_allocation");
    content.can_accept=false;
    check(text_form_update(&scene,content,viewport,&cache)&&text_form_focused(scene)==TextFormElement::cancel&&
        !text_form_focus(&scene,TextFormElement::accept)&&text_form_move_focus(&scene,false)&&
        text_form_focused(scene)==TextFormElement::cancel,"confirmation_disabled_accept_keeps_cancel_keyboard_target");
    content.can_accept=true;
    const UiRect sizes[]{{0,0,320,200},{13,27,173,227},{0,0,96,64}};
    for(const auto size:sizes){const bool updated=text_form_update(&scene,content,size,&cache);
        check(updated,"confirmation_compact_native_viewport_update");if(!updated)continue;
        check(ui_layout_engine_validate(scene.widgets.layout,{diagnostics,32}).clean()&&
            text_form_focus(&scene,TextFormElement::accept),"confirmation_compact_layout_reveals_explicit_accept");
        UiRect anchor{};check(ui_widget_manager_context_anchor(scene.widgets,&anchor)&&
            text_form_hit_test(scene,(anchor.left+anchor.right)*.5f,(anchor.top+anchor.bottom)*.5f)==TextFormElement::accept,
            "confirmation_revealed_accept_has_real_hit_target");
        check(text_form_scroll(&scene,-1000000),"confirmation_context_is_accessible_through_native_scroll");
        PaintStream paint{};UiTextCommand commands[16]{};UiTextCommandStream text{};
        UiTextOutput text_output{&text, nullptr};
        check(paint_stream_reset(&paint,size.right,size.bottom)&&text_form_paint(scene,{},kDefaultTheme,&paint)&&
            paint.clip_depth==1&&paint.rejected==0,"confirmation_paints_without_editor_geometry");
        check(ui_text_command_stream_initialize(&text,{commands,16})&&text_form_text_output(scene,{},kDefaultTheme,&text_output),
            "confirmation_text_needs_no_editor_projection");
        paint_stream_release(&paint);
    }
    TextFormContent editing=content;editing.confirmation_only=false;editing.label=utf8_literal("Description");editing.text=utf8_literal("retained text");
    const auto old_cancel=scene.elements[static_cast<u32>(TextFormElement::cancel)];
    check(text_form_update(&scene,editing,viewport,&cache)&&text_form_node(scene,TextFormElement::edit)&&
        text_form_node(scene,TextFormElement::label)&&text_form_focused(scene)==TextFormElement::edit&&
        scene.storage.arena.base==allocation&&!ui_widget_manager_resolve(scene.widgets,old_cancel),
        "confirmation_switch_to_existing_edit_mode_reuses_storage_and_invalidates_handles");
    editing.confirmation_only=true;
    check(text_form_update(&scene,editing,viewport,&cache)&&!text_form_node(scene,TextFormElement::edit)&&
        text_form_focused(scene)==TextFormElement::cancel,"confirmation_switch_back_restores_no_editor_cancel_focus");
    // The "Leaving Discord" option row: a checkbox square left of a wrapped
    // label, focusable before the buttons, and painted checked on demand.
    TextFormContent option=content;option.option=utf8_literal("Trust example.com links from now on");
    check(text_form_update(&scene,option,viewport,&cache)&&scene.widgets.count==kTextFormElements-2-kTextFormChoiceElements&&
        text_form_node(scene,TextFormElement::option)&&text_form_focused(scene)==TextFormElement::cancel,
        "confirmation_option_row_builds_and_keeps_cancel_focus");
    const auto* row=text_form_node(scene,TextFormElement::option);const UiRect box=text_form_option_box(scene);
    const auto* actions=text_form_node(scene,TextFormElement::actions);
    check(row&&actions&&ui_rect_width(box)==20&&ui_rect_height(box)==20&&box.left==row->rectangle.left&&
        row->content_rectangle.left>=box.right+8&&row->rectangle.bottom<=actions->rectangle.top&&
        text_form_hit_test(scene,(box.left+box.right)*.5f,(box.top+box.bottom)*.5f)==TextFormElement::option,
        "confirmation_option_box_sits_left_of_its_label_above_the_buttons");
    check(text_form_move_focus(&scene,true)&&text_form_focused(scene)==TextFormElement::option&&
        text_form_move_focus(&scene,false)&&text_form_focused(scene)==TextFormElement::cancel,
        "confirmation_option_is_in_the_tab_order");
    u32 painted[2]{};
    for(u32 checked=0;checked<2;++checked){option.option_checked=checked!=0;const u32 revision=scene.revision;
        PaintStream option_paint{};UiTextCommand option_commands[16]{};UiTextCommandStream option_text{};
        UiTextOutput option_text_output{&option_text, nullptr};
        check(text_form_update(&scene,option,viewport,&cache)&&(!checked||scene.revision!=revision)&&
            paint_stream_reset(&option_paint,viewport.right,viewport.bottom)&&text_form_paint(scene,{},kDefaultTheme,&option_paint)&&
            option_paint.rejected==0&&ui_text_command_stream_initialize(&option_text,{option_commands,16})&&
            text_form_text_output(scene,{},kDefaultTheme,&option_text_output),"confirmation_option_paints_and_projects_text");
        painted[checked]=option_paint.count;bool label=false;
        for(u32 i=0;i<option_text.count;++i)label=label||(option_commands[i].text.bytes==option.option.bytes&&
            bytes_equal(option_commands[i].text.data,option.option.data,option.option.bytes)&&
            option_commands[i].rectangle.left==row->content_rectangle.left);
        check(label,"confirmation_option_label_is_drawn_beside_the_box");
        paint_stream_release(&option_paint);}
    check(painted[1]==painted[0]+2,"confirmation_checked_option_adds_the_check_mark");
    check(text_form_update(&scene,content,viewport,&cache)&&!text_form_node(scene,TextFormElement::option)&&
        scene.widgets.count==kTextFormElements-3-kTextFormChoiceElements,"confirmation_option_row_is_removed_with_its_label");
    text_form_release(&scene);check(!scene.storage.arena.base&&!scene.active,"confirmation_release_frees_native_scene");
}
}

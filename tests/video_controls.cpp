#include "maicord/ui/video_controls.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool time_is(const UiVideoPlaybackState& state, Utf8View expected) noexcept {
    char text[64]{};
    const u32 bytes = ui_video_format_time(state, text, sizeof(text));
    return utf8_equal({text, bytes}, expected);
}
void test_video_player_keys(void (*check)(bool,const char*) noexcept) noexcept {
    UiVideoPlaybackState state{7,8,9,30000,120000,40,true,false,false,false,true,true,true,true};
    UiVideoControlsStyle style{}; UiVideoControlsProjection projection{};
    const auto project=[&]() noexcept {
        return ui_video_controls_project(state,{0,0,640,32},style,kDefaultTheme,&projection);
    };
    UiVideoAction action{};
    check(project()&&ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_forward,0,&action)&&
        action.control==UiVideoControl::seek&&action.position_ms==35000&&ui_video_action_current(state,action)&&
        ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_backward,0,&action)&&action.position_ms==25000,
        "video keys: arrows request exact five-second fenced seeks without mutating elapsed time");
    state.elapsed_ms=3000; project();
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_backward,0,&action)&&!action.position_ms,
        "video keys: backward seek clamps at the start");
    state.elapsed_ms=118000; project();
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_forward,0,&action)&&action.position_ms==120000,
        "video keys: forward seek clamps at the duration");
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_percent,50,&action)&&action.position_ms==60000&&
        ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_percent,0,&action)&&!action.position_ms&&
        ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_percent,90,&action)&&action.position_ms==108000&&
        !ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_percent,101,&action),
        "video keys: digit N seeks to N tenths of the actual duration and rejects out-of-range percentages");
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::toggle_play,0,&action)&&
        action.control==UiVideoControl::play_pause&&!action.value&&
        ui_video_controls_player_key(projection,UiVideoPlayerKey::toggle_mute,0,&action)&&
        action.control==UiVideoControl::mute&&action.value,
        "video keys: Space and M request the opposite of the actual play and mute state");
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::volume_up,0,&action)&&action.volume==45&&
        ui_video_controls_player_key(projection,UiVideoPlayerKey::volume_down,0,&action)&&action.volume==35,
        "video keys: vertical arrows step the actual volume");
    state.volume=3; project();
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::volume_down,0,&action)&&!action.volume,
        "video keys: volume steps clamp at silence");
    check(!ui_video_controls_player_key(projection,UiVideoPlayerKey::expand,0,&action),
        "video keys: F needs a host expansion capability");
    state.can_expand=true; project();
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::expand,0,&action)&&
        action.control==UiVideoControl::expand&&action.value,"video keys: F requests expansion");
    state.expanded=true; project();
    check(ui_video_controls_player_key(projection,UiVideoPlayerKey::expand,0,&action)&&!action.value&&
        utf8_equal(ui_video_controls_label(projection,UiVideoControl::expand),style.collapse),
        "video keys: F in the expanded player requests the inline player");
    state.can_seek=false; project();
    check(!ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_forward,0,&action)&&
        !ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_percent,10,&action)&&
        !ui_video_controls_player_key(projection,UiVideoPlayerKey::seek_forward,0,nullptr),
        "video keys: seek shortcuts follow the seek capability");
}
void test_video_time_and_expand(void (*check)(bool,const char*) noexcept) noexcept {
    UiVideoPlaybackState state{7,8,9,61000,3723000,40,true,false,false,false,true,true,true,true};
    check(time_is(state,utf8_literal("0:01:01 / 1:02:03")),"video time: hour-long media pads elapsed time to h:mm:ss");
    state.duration_ms=0; state.elapsed_ms=0;
    check(time_is(state,utf8_literal("-:-- / -:--")),"video time: unknown metadata shows placeholders");
    state.elapsed_ms=5000;
    check(time_is(state,utf8_literal("0:05 / -:--")),"video time: known elapsed time survives an unknown duration");
    state.elapsed_ms=599000; state.duration_ms=600000;
    check(time_is(state,utf8_literal("9:59 / 10:00")),"video time: minutes are unpadded below an hour");
    UiVideoControlsStyle style{}; UiVideoControlsProjection projection{};
    state.duration_ms=36000000; state.elapsed_ms=0;
    check(ui_video_controls_project(state,{0,0,640,32},style,kDefaultTheme,&projection)&&
        ui_rect_width(projection.controls[2].bounds)>style.time_width&&
        projection.controls[2].bounds.right<projection.controls[1].bounds.left,
        "video time: the time slot widens for h:mm:ss instead of overlapping the seek range");
    state.duration_ms=120000; state.can_expand=true;
    check(ui_video_controls_project(state,{0,0,640,32},style,kDefaultTheme,&projection)&&
        projection.controls[6].visible&&projection.controls[6].enabled&&projection.controls[6].bounds.right==640&&
        projection.controls[3].bounds.right==projection.controls[6].bounds.left,
        "video expand: the expand target sits at the trailing edge after the speaker");
    UiVideoAction action{}; const auto expand=projection.controls[6].bounds;
    check(ui_video_controls_pointer_action(projection,{(expand.left+expand.right)*.5f,16},&action)&&
        action.control==UiVideoControl::expand&&action.value,
        "video expand: pointer activation requests expansion through the fenced action");
    UiVideoControl focus=UiVideoControl::volume; action={};
    check(ui_video_controls_key(projection,UiVideoKey::next,&focus,&action)&&focus==UiVideoControl::expand&&
        ui_video_controls_key(projection,UiVideoKey::activate,&focus,&action)&&action.control==UiVideoControl::expand,
        "video expand: Tab reaches expand after volume and Enter activates it");
    check(ui_video_controls_project(state,{0,0,200,32},style,kDefaultTheme,&projection,true)&&
        !projection.controls[6].visible&&!projection.controls[6].enabled&&
        ui_rect_width(projection.controls[1].bounds)>=style.range_height,
        "video expand: a narrow player drops the optional expand slot before the seek range");
}
} // namespace
void test_video_controls(void (*check)(bool,const char*) noexcept) noexcept {
    UiVideoPlaybackState state{7,8,9,30000,120000,40,true,false,false,false,true,true,true,true};
    UiVideoControlsStyle style{}; UiVideoControlsProjection projection{};
    const auto project=[&](float width) noexcept {
        return ui_video_controls_project(state,{10,20,10+width,20+ui_video_controls_height(style,width)},style,kDefaultTheme,&projection,true);
    };
    check(project(640)&&!projection.compact&&projection.controls[1].value==2500&&
        utf8_equal(ui_video_controls_label(projection,UiVideoControl::time),utf8_literal("0:30 / 2:00")),
        "video controls: actual playback projects normal geometry and elapsed/duration without a clock");
    check(ui_video_controls_height(style,640)==kUiVideoControlBarHeight&&
        ui_rect_width(projection.controls[0].bounds)==kUiVideoControlBarHeight&&projection.controls[0].bounds.left==10,
        "video controls: the official 32 DIP bar uses full-height edge-aligned targets");
    const auto copied=projection;
    projection.time_text[0]='9';
    check(ui_video_controls_label(copied,UiVideoControl::time).data==copied.time_text&&
        utf8_equal(ui_video_controls_label(copied,UiVideoControl::time),utf8_literal("0:30 / 2:00")),
        "video controls: copied projection time accessor owns its own buffer");
    check(project(256)&&projection.compact,"video controls: minimum compact player retains every descriptor");
    bool contained=true;
    for(const auto& c:projection.controls) if(c.visible) contained=contained&&c.bounds.left>=projection.bounds.left&&
        c.bounds.right<=projection.bounds.right&&c.bounds.top>=projection.bounds.top&&c.bounds.bottom<=projection.bounds.bottom;
    check(contained&&projection.controls[2].visible&&projection.controls[4].visible&&
        projection.controls[4].bounds.right==projection.controls[3].bounds.left&&
        projection.controls[1].bounds.right<=projection.controls[4].bounds.left&&
        ui_video_controls_height(style,256)==kUiVideoControlBarHeight,
        "video controls: the expanded slider opens beside the speaker while time and seek share the row");
    check(project(200)&&!projection.controls[2].visible&&
        projection.controls[4].bounds.left==projection.controls[2].bounds.left&&
        projection.controls[4].bounds.right<=projection.controls[1].bounds.left&&
        ui_rect_width(projection.controls[1].bounds)>=style.range_height&&project(256),
        "video controls: a narrow player hands the time slot to the slider instead of starving seek");
    UiVideoControlsProjection collapsed{};
    check(ui_video_controls_project(state,{0,0,240,40},style,kDefaultTheme,&collapsed)&&
        collapsed.controls[2].visible&&!collapsed.controls[4].visible&&collapsed.controls[4].enabled&&
        ui_video_controls_hit(collapsed,{collapsed.controls[4].bounds.left+1,20})!=UiVideoControl::volume,
        "video controls: narrow collapsed player keeps time and keyboard volume without an invisible pointer target");
    UiVideoAction action{}; const auto play=projection.controls[0].bounds;
    check(ui_video_controls_pointer_action(projection,{(play.left+play.right)*.5f,(play.top+play.bottom)*.5f},&action)&&
        action.control==UiVideoControl::play_pause&&!action.value&&state.playing&&ui_video_action_current(state,action),
        "video controls: pointer pause emits a fenced request without mutating playback");
    const auto timeline=projection.controls[1].bounds;
    check(ui_video_controls_pointer_action(projection,{(timeline.left+timeline.right)*.5f,(timeline.top+timeline.bottom)*.5f},&action)&&
        action.position_ms==60000&&state.elapsed_ms==30000,
        "video controls: pointer timeline uses exact duration and leaves elapsed state authoritative");
    UiVideoControl focus=UiVideoControl::none;
    action={UiVideoControl::volume,state.owner,state.generation,state.revision,0,100,false};
    check(ui_video_controls_key(projection,UiVideoKey::next,&focus,&action)&&focus==UiVideoControl::play_pause&&
        action.control==UiVideoControl::none&&!ui_video_action_current(state,action),
        "video controls: successful focus-only navigation clears a poisoned prior request before generic host dispatch");
    check(
        ui_video_controls_key(projection,UiVideoKey::next,&focus,&action)&&focus==UiVideoControl::seek&&
        ui_video_controls_key(projection,UiVideoKey::increase,&focus,&action)&&action.position_ms==36000&&
        ui_video_controls_key(projection,UiVideoKey::end,&focus,&action)&&action.position_ms==120000,
        "video controls: compact keyboard focus and range requests share semantic actions");
    focus=UiVideoControl::volume;
    check(ui_video_controls_key(projection,UiVideoKey::decrease,&focus,&action)&&action.volume==35&&
        ui_video_controls_key(projection,UiVideoKey::home,&focus,&action)&&!action.volume,
        "video controls: volume keyboard requests are bounded actual percentage values");
    ++state.revision;
    check(!ui_video_action_current(state,action),"video controls: changed caller interaction revision rejects an outstanding action");
    state.has_audio=false; state.duration_ms=0; state.can_seek=false; state.buffering=true;
    check(project(256)&&!projection.controls[1].enabled&&!projection.controls[3].enabled&&!projection.controls[4].enabled&&
        utf8_equal(ui_video_controls_label(projection,UiVideoControl::time),utf8_literal("0:30 / -:--"))&&
        utf8_equal(ui_video_controls_label(projection,UiVideoControl::status),style.buffering),
        "video controls: unknown duration, absent audio and actual buffering remain explicit");
    focus=UiVideoControl::play_pause;
    check(ui_video_controls_key(projection,UiVideoKey::next,&focus,&action)&&focus==UiVideoControl::play_pause&&
        !ui_video_controls_pointer_action(projection,{timeline.left,timeline.top},&action),
        "video controls: disabled ranges skip focus and reject pointer activation");
    state.error=true;
    check(project(640)&&!ui_video_controls_key(projection,UiVideoKey::next,&focus,&action)&&focus==UiVideoControl::none&&
        utf8_equal(ui_video_controls_label(projection,UiVideoControl::status),style.error)&&
        projection.controls[5].foreground.red==kDefaultTheme.notification.red,
        "video controls: error disables interaction and uses semantic error feedback");
    const auto before=projection;
    state.volume=101;
    check(!project(640)&&projection.playback.volume==before.playback.volume,
        "video controls: invalid actual values reject without replacing the usable projection");
    state.volume=40; state.error=false; state.duration_ms=1000;
    check(!project(640)&&!ui_video_controls_project(state,{0,0,255,100},style,kDefaultTheme,&projection),
        "video controls: out-of-duration elapsed and undersized layout reject");
    state.duration_ms=120000; style.gap=-1;
    check(!project(640),"video controls: invalid caller metrics reject before publishing geometry");
    style={}; const u32 infinity_bits=0x7f800000; float nonfinite{}; copy_bytes(&nonfinite,&infinity_bits,sizeof(nonfinite));
    check(!ui_video_controls_project(state,{0,0,nonfinite,100},style,kDefaultTheme,&projection)&&
        ui_video_controls_hit(before,{nonfinite,0})==UiVideoControl::none,
        "video controls: nonfinite geometry never produces an actionable target");
    test_video_player_keys(check);
    test_video_time_and_expand(check);
}
}

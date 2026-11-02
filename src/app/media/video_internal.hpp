#pragma once
// Shared by video.cpp and its parts (video_*.cpp); private to them.
#include "downloads.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/window_state.hpp"
#include "../shell/content_point.hpp"
#include "video_request.hpp"
#include "../shell/state.hpp"
#include "../chat/channel_authority.hpp"
#include "../chat/search/search.hpp"
#include "image_viewer.hpp"
#include "../shell/cursor.hpp"
#include "maicord/media/video/gpu_video_frame.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/encoding/base64.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/media/account_video_playback.hpp"

namespace maicord::app_detail {
namespace video_detail {}
using namespace video_detail;

// No default member initializers: `InlineMediaSource source{}` is one zero fill.
struct InlineMediaSource {
    MediaAssetKey key;
    UiRect preview;
    UiRect clip;
    u64 message_id;
    u64 channel_id;
    u32 revision;
    u8 index;
    bool embed;
    bool snapshot;
    ApplicationSearchSourceTicket search_source;
    UiWidgetReference owner;
    bool audio;
    bool provider;
    u32 start_seconds;
    bool audio_preview;
};
struct InlineAudioSceneSelection {};
struct InlineVideoSceneSelection {};
union InlinePlaybackSceneStorage {
    UiVideoScene video;
    UiAudioScene audio;
    explicit InlinePlaybackSceneStorage(InlineAudioSceneSelection) noexcept : audio{} {
    }
    explicit InlinePlaybackSceneStorage(InlineVideoSceneSelection) noexcept : video{} {
    }
    ~InlinePlaybackSceneStorage() noexcept {
    }
};
struct ApplicationVideo {
    InlinePlaybackSceneStorage ui;
    char audio_title[512]{};
    u8 audio_waveform[kUiAudioWaveformMaximum]{};
    u32 audio_title_bytes{}, audio_waveform_bytes{}, audio_declared_duration_ms{};
    bool audio_duration_known{}, audio_voice{};
    InlineMediaSource source{};
    AccountVideoPlaybackStatus status{};
    AccountVideoIdentity retiring_identity{};
    u64 account_serial{};
    u64 session_epoch{};
    u64 command_serial{};
    u64 presentation_wake_ms{};
    bool selecting{};
    bool selected{};
    bool requested_play{};
    bool requested_seek{};
    bool provider_frame_ready{};
    u32 display_width{}, display_height{};
    bool hidden_pause{};
    bool input_focus{};
    bool presentation_failed{};
    // The media lane reported an expired signed URL; the next source request forces a refresh.
    bool source_refresh{};
    // The media viewer hosts this same session; geometry comes from the viewer.
    bool expanded{};
    bool live_started{}, live_restart{}, restore_gain{}, restore_muted{};
    u8 live_restarts{};
    float restore_volume{1};
    u16 collection_count{}, collection_track{};
    bool collection_more{};
    bool collection_continue{true};
    UiVideoControl collection_focus{};
    UiPoint pointer{-kMaximumUiCoordinate, -kMaximumUiCoordinate};
    static void* operator new(usize, void* storage) noexcept {
        return storage;
    }
    static void operator delete(void*, void*) noexcept {
    }
    // MSVC emits a scalar deleting destructor even for this placement-only owner.
    // Its valid cleanup is explicit destruction followed by VirtualFree.
    static void operator delete(void*, usize) noexcept {
        __fastfail(FAST_FAIL_INVALID_ARG);
    }
    explicit ApplicationVideo(InlineAudioSceneSelection selected_scene) noexcept : ui(selected_scene) {
        source.audio = true;
    }
    explicit ApplicationVideo(InlineVideoSceneSelection selected_scene) noexcept : ui(selected_scene) {
    }
    ~ApplicationVideo() noexcept {
        if (source.audio)
            ui.audio.~UiAudioScene();
        else
            ui.video.~UiVideoScene();
    }
};

namespace video_detail {

AccountPlaybackKind audio_playback_kind(MediaAttachmentFormat format) noexcept;
bool content_surface_covers_video() noexcept;

// Capture and repaint requests every scene event can carry.
template <typename Result> void apply_scene_result(const Result& event) noexcept {
    if (event.request_capture)
        SetCapture(g_app.window);
    if (event.release_capture)
        application_release_capture();
    if (event.handled)
        application_request_frame();
}

bool unbind_playback_scenes() noexcept;
bool video_session_available() noexcept;
bool video_surface_available() noexcept;
UiPoint video_pointer_position() noexcept;
void note_video_pointer(ApplicationVideo& video, UINT message, UiPoint point) noexcept;
bool video_input_focus_current(const ApplicationVideo& video) noexcept;
void acquire_video_focus(ApplicationVideo& video) noexcept;
bool resolve_search_video_source(const ApplicationSearchSourceTicket& ticket, bool embed, u8 item,
                                 bool snapshot, InlineMediaSource* out) noexcept;
bool resolve_visible_video_source(u64 message_id, u32 revision, bool embed, u8 item, InlineMediaSource* out,
                                  bool snapshot = false,
                                  const ApplicationSearchSourceTicket* search_source = nullptr) noexcept;
bool request_video_source(const InlineMediaSource& source, bool* refresh = nullptr, ApplicationVideo* playback = nullptr) noexcept;
bool send_video_command(ApplicationVideo& video, AccountVideoCommand kind, i64 seek = 0, float volume = 1,
                        bool muted = false) noexcept;
bool set_video_expanded(ApplicationVideo& video, bool expanded) noexcept;

enum class InlineMediaPolicyUse : u8 {
    activate,
    cursor,
    retain
};

void dispatch_video_event(ApplicationVideo& video, const UiVideoSceneResult& event) noexcept;
bool start_video_session(const InlineMediaSource& source) noexcept;
bool switch_collection_track(ApplicationVideo&, u16 track, UiVideoControl focus) noexcept;
bool restart_live_session(ApplicationVideo& video) noexcept;
bool inline_media_activation_consumed(const InlineMediaSource& source,
                                      InlineMediaPolicyUse use = InlineMediaPolicyUse::activate) noexcept;
bool activate_inline_source(const InlineMediaSource& source) noexcept;
void dispatch_audio_event(ApplicationVideo& video, const UiAudioSceneResult& event) noexcept;
bool search_video_still_current(const ApplicationVideo& video) noexcept;
bool visible_source_at(UiPoint point, InlineMediaSource* source, const MessageRichRecord** rich_out,
                       u32* item_out) noexcept;
bool video_scene_pointer_event(ApplicationVideo& video, UINT message, UiPoint point) noexcept;
void close_video_session(AccountVideoIdentity* retired_identity) noexcept;
bool position_video_scene(ApplicationVideo* video, InlineMediaSource& positioned) noexcept;
bool update_video_extent(ApplicationVideo& video, const GpuVideoFrame& frame) noexcept;
bool project_video_scene(ApplicationVideo* video, const InlineMediaSource& positioned, bool visible, bool* repaint) noexcept;
__declspec(noinline) void refresh_video_status(ApplicationVideo& video) noexcept;

} // namespace video_detail
} // namespace maicord::app_detail

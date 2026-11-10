#pragma once
#include "maicord/media/video/stream_quality.hpp"
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/protocol/gateway/stream.hpp"

namespace maicord {
// Go Live sending: the UI's view of the local user's own stream. The session
// lives in the account module on the gateway worker and owns the capture, the
// encoder, stream audio and the stream server connection; the UI reaches it
// through AccountModuleApi::go_live / go_live_snapshot / go_live_preview.
//
//   start -> STREAM_CREATE (op 18) -> STREAM_CREATE + STREAM_SERVER_UPDATE ->
//   stream voice connection (sender: video SSRCs from Ready, op 12, DAVE) ->
//   capture -> encode -> RTP -> live; stop -> STREAM_DELETE (op 19).
// Pause (op 22) follows the shared window: minimized pauses, restored resumes.

// The quality the UI resolved with stream_preset_quality (Nitro and boosts
// applied): resolution is a picture height or 0 for the source's own size.
struct GoLiveSettings {
    u16 resolution;
    u8 fps;
    StreamPreset preset;
    bool sound; // Share the source's audio.
    u8 reserved[3];
};
static_assert(sizeof(GoLiveSettings) == 8);

enum class GoLiveCommandKind : u8 {
    start,         // guild_id, channel_id, settings, source/system_picker. Ends any current stream first.
    stop,          // STREAM_DELETE; the snapshot ends with GoLiveEnd::stopped.
    update,        // New settings while live (quality, sound).
    change_source, // A new source while live; the stream continues.
    pause,         // Op 22 paused, video and audio stop.
    resume,
};
// Crosses the account module ABI.
struct GoLiveCommand {
    CaptureSource source;
    u64 guild_id, channel_id; // guild 0: a private call.
    GoLiveSettings settings;
    GoLiveCommandKind kind;
    bool system_picker; // CaptureSupport::system_picker: the system asks; source is ignored.
    u8 reserved[6];
};
static_assert(sizeof(GoLiveCommand) == 248);

enum class GoLiveState : u8 {
    idle,
    starting,     // STREAM_CREATE sent; waiting for the stream server.
    connecting,   // The stream connection negotiates (DAVE included).
    live,         // Media flows.
    reconnecting, // The stream connection is reconnecting.
    ended,        // See GoLiveEnd.
};
enum class GoLiveEnd : u8 {
    none,
    stopped,             // Local stop.
    no_hardware_encoder, // The GPU has no hardware encoder the stream accepts (no H.264: cannot stream).
    capture_unavailable, // Nothing can capture on this system, or the source could not be captured.
    capture_denied,      // The system picker was cancelled or capture was refused.
    source_closed,       // The shared window closed or the screen went away.
    not_allowed,         // The server refused the stream (permissions).
    stream_full,
    connection_failed,   // The stream server connection failed for good.
    timeout,             // No STREAM_CREATE / STREAM_SERVER_UPDATE in time.
    deleted,             // STREAM_DELETE for another reason (delete_reason).
    left_channel,        // The call left the stream's channel.
    encoder_failed,      // The encoder or the GPU stopped working.
};
inline constexpr u32 kGoLiveViewerIds = 32;
// Crosses the account module ABI.
struct GoLiveSnapshot {
    GatewayStreamKey key;
    u64 generation; // Bumps whenever anything below changes.
    u64 viewer_ids[kGoLiveViewerIds];
    u32 viewer_count; // May exceed kGoLiveViewerIds.
    u32 encodable;    // voice_video_codec_bit set the GPU encodes in hardware; 0 until known or none (no H.264).
    GoLiveSettings settings;
    GoLiveState state;
    GoLiveEnd end;
    bool paused;
    bool encodable_known; // encodable was probed (the renderer device is set).
    u8 reserved[4];
};
static_assert(sizeof(GoLiveSnapshot) == 320);
} // namespace maicord

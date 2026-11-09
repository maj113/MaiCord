# Screen, window and stream audio capture (Go Live)

What a Go Live stream shows and plays: `include/maicord/platform/capture/screen_capture.hpp` (the source list, picker
thumbnails, and the frames of a running capture) and `include/maicord/platform/audio/stream_audio.hpp` (what the computer
plays, for the stream's Opus encoder). Library `maicord_capture`. The pure rules both systems share (which windows the
picker lists, the thumbnail steps, the frame cap, cut names) are `capture_policy.hpp`.

## The frame contract

A frame is valid during `CaptureSink::frame` only, on the capture's own thread. The consumer (the Go Live encoder)
enqueues its GPU reads before it returns and keeps nothing. Nothing is copied through the CPU:

- **Windows:** a BGRA texture on the renderer's multithread-protected device. The consumer's reads are queued on the
  same immediate context before the buffer goes back to the system, so they come before the next write.
- **Linux:** the compositor's dma-buf (one plane of packed RGB: XR24, AR24, XB24 or AB24, linear or with the implicit
  modifier) with its pitch and offset. The dma-buf's implicit fences order the consumer's GPU reads.

`timestamp_us` is the client clock (`clock_microseconds(clock_initialize(), 0, clock_now())`) at composition: the
capture's own time where the system gives one (WGC's SystemRelativeTime, Desktop Duplication's LastPresentTime, the
PipeWire header meta's pts), arrival time otherwise. Stream audio blocks carry the same clock. Frames come only when the
picture changes, and no closer together than `max_fps` allows (`CapturePacer`, which keeps a source at the cap intact
through jitter).

## Windows

| Build | Screens | Windows | Cursor | Stream audio |
| --- | --- | --- | --- | --- |
| 1809 to 1909 (17763-18363) | DXGI Desktop Duplication, cursor drawn in with DrawIconEx | WGC, with the yellow border | always in the picture (WGC) | none: no loopback can leave MaiCord out |
| 2004 to 21H2 of Windows 10 (19041-19044) | Desktop Duplication | WGC, with the border | can be left out (IsCursorCaptureEnabled) | process loopback: a window's process tree, or everything but MaiCord's tree |
| Server 2022, Windows 11 (20348+) | WGC, borderless | WGC, borderless | can be left out | process loopback |
| 24H2 (26100+) | as above; the frame cap is also the system's (MinUpdateInterval) | | | |

- **WGC** (Windows.Graphics.Capture) is called through its ABI by vtable slot (`windows/capture_abi.hpp`, beside the
  toast path's helpers), with no projection headers: GraphicsCaptureItem from the HWND or HMONITOR, the free-threaded
  frame pool (`CreateFreeThreaded`, in 1809) on the renderer's device, `FrameArrived` setting the capture thread's event.
  A window that resizes recreates the pool at its new size; the frame says which part of the texture is the picture.
  Borderless capture asks `GraphicsCaptureAccess.RequestAccessAsync(Borderless)` once (answered at once for a desktop
  application) before `IsBorderRequired(false)`.
- **Desktop Duplication** serves screens where the border cannot be turned off. The desktop image is copied on the GPU
  into a GDI-compatible texture (the cursor is drawn into that one, never into the system's). A screen duplication cannot
  serve (another adapter drives it, a rotated mode) goes to WGC with its border; the secure desktop and mode changes
  (access lost) are waited out.
- **Thumbnails** never show a border: WGC where it is borderless, Desktop Duplication for screens before that, and
  `PrintWindow(PW_RENDERFULLCONTENT)` into a GDI-compatible texture for windows before that. The picture is reduced on
  the GPU with GenerateMips to the first level that fits, and that level alone is read back as RGBA.
- **Sources:** screens first (the primary first, "Screen N"), then visible, uncloaked, titled, unowned top-level windows
  that are not tool windows, the desktop, or MaiCord's.
- **Minimized windows** send no frames; `CaptureSink::minimized` reports each change (the stream pauses meanwhile, as
  Discord's does).
- **Stream audio:** WASAPI process loopback, 48 kHz stereo PCM16 converted by the engine, stamped with each packet's
  QPC time: the `VAD\Process_Loopback` device activated through `ActivateAudioInterfaceAsync` (mmdevapi.dll, resolved at
  run time) with `AUDIOCLIENT_ACTIVATION_PARAMS`. Before 19041 there is none: the only loopback there is the device's
  whole mix, which carries the call (subtracting what MaiCord wrote leaves a residue, since the engine applies session
  volumes and effects first), so `stream_audio_method()` reports `unavailable_windows_version` and the UI says why.

## Linux

- **Picker:** the xdg-desktop-portal ScreenCast interface over a session-bus connection of the capture thread's own
  (`src/platform/dbus`): CreateSession, SelectSources (the kinds asked for, one source, the cursor drawn in or hidden
  where the portal offers the mode), Start (the portal's own picker; a cancel ends the capture as denied),
  OpenPipeWireRemote. MaiCord lists no sources and makes no thumbnails there. Session.Closed (the user stopped sharing
  from the desktop) ends the capture as closed.
- **PipeWire:** `libpipewire-0.3.so.0` is `dlopen`ed (`include/maicord/platform/capture/pipewire/abi.hpp`, checked by
  `tools/check_pipewire_abi.py`): absent means capture is unavailable. The consumer offers packed RGB as dma-bufs only,
  with the linear and implicit modifiers as a mandatory choice the producer narrows and the consumer fixes (PipeWire's
  dma-buf negotiation), and asks for the header meta. A compositor that can only give shared memory negotiates nothing
  and the capture ends as unsupported: pictures are never copied through the CPU. The newest buffer of each cycle is
  handed over; older ones go straight back. A stream that does not reach the portal's node within 10 s fails.
- **Stream audio:** a PipeWire capture stream (48 kHz stereo S16LE, not autoconnected: a session manager would link it
  to a microphone) that MaiCord links itself, through the link factory, to the output ports of every playback stream
  (`media.class` Stream/Output/Audio) whose `application.process.id` is not MaiCord's (`linux/stream_audio_routes.hpp`):
  left to left, right to right, mono and centre to both. The registry keeps the set current as streams come and go, and
  PipeWire mixes what is linked, after each stream's own volume. MaiCord's audio reaches PipeWire through pipewire-pulse
  with its process id, so the call is never in it. Without libpipewire or a daemon socket `stream_audio_method()`
  reports `unavailable_no_pipewire`: never a sink monitor, which would carry the call.

## Tests

- `maicord_capture` (both): the picker, thumbnail, frame-cap and name rules. Windows: the WGC and process loopback ABI
  against the SDK (`tests/platform/capture/capture_abi.cpp`, SDK includes: IIDs, slots, class names, constants, the
  activation parameters' layout), the methods per build, the real source list's order (read, never captured or printed),
  and the GPU thumbnail of a synthetic texture. Linux: the negotiation PODs and the audio format byte for byte against
  libspa's builder, the format reader against hostile input, the fourccs, and the stream audio routing rules.
- `maicord_capture_portal` (Linux, `tools/test_dbus.sh`): the portal flow against the ScreenCast mock of
  `tests/platform/dbus/mock_services.py` (the options of each request, the cancel, a remote without PipeWire, the
  session closed every time, a stop during the dialog); `_bare`: no portal, unsupported.
- `maicord_capture_pipewire` (Linux, `tools/test_capture_pipewire.sh`): the consumer against a private PipeWire daemon
  and a synthetic dma-buf source (dumb buffers of `/dev/dri/card0`), linked by the script as a session manager would:
  the modifier fixed, every frame the source's own dma-buf with its layout, timestamp and pattern, and no frame or
  failure once the source goes.
- `maicord_stream_audio_pipewire` (Linux, the same script): stream audio against the private daemon (with a dummy
  driver) and two synthetic players started after the capture, one with MaiCord's process id: only the other is heard,
  on both channels, at its level.
- `maicord_capture_probe` (both, opt-in `MAICORD_CAPTURE_PROBE=1`): the real primary screen (the portal's picker on
  Linux) for two seconds and two seconds of stream audio, printing counts and sizes only.

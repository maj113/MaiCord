#include "maicord/app/account_soundboard.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/voice_mixer.hpp"
#include "maicord/app/voice_playback.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
static_assert(kSoundboardMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kSoundboardMaximumSmallBodyBytes <= kRestClientMaximumRequestBodyBytes);
static_assert(kSoundboardCdnTargetBytes <= kCdnMaximumTargetBytes);

namespace {
SoundboardFileInfo check_file(const SoundboardRequest& request) noexcept {
    auto* scratch = static_cast<u8*>(
        VirtualAlloc(nullptr, kSoundboardProbeScratchBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    SoundboardFileInfo info{};
    if (!scratch)
        info.error = SoundboardFileError::memory;
    else {
        info = soundboard_probe_file(request.sound, {scratch, kSoundboardProbeScratchBytes});
        VirtualFree(scratch, 0, MEM_RELEASE);
    }
    if (request.info)
        *request.info = info;
    return info;
}

__declspec(noinline) RestClientSubmitResult submit_create(RestClient* rest,
                                                          const SoundboardRequest& request,
                                                          ConstSlice<u8> target, ConstSlice<u8> reason,
                                                          u64 request_id) noexcept {
    const auto info = check_file(request);
    if (!info.ok() || info.format != request.format)
        return {RestClientSubmitError::invalid_input};
    const u32 bytes = soundboard_body_bytes(request);
    if (!bytes)
        return {RestClientSubmitError::invalid_input};
    if (bytes > kRestClientMaximumLargeRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = soundboard_write_body(request, {body, bytes});
    RestClientSubmitResult result{RestClientSubmitError::body_capacity};
    if (written.ok() && written.bytes == bytes) {
        const bool large = bytes > kRestClientMaximumRequestBodyBytes;
        const RestClientRequest submit{target,
                                       {body, bytes},
                                       soundboard_route(request),
                                       request_id,
                                       large ? kSoundboardLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                                       true,
                                       false,
                                       reason};
        result = large ? rest_client_submit_large(rest, submit) : rest_client_submit(rest, submit);
    }
    secure_clear_bytes(body, bytes);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}
} // namespace

bool account_soundboard_probe(const SoundboardRequest* request) noexcept {
    if (!request || request->operation != SoundboardOperation::probe || !soundboard_request_valid(*request))
        return false;
    return check_file(*request).ok();
}

RestClientSubmitResult account_soundboard_submit(RestClient* rest, const SoundboardRequest* request,
                                                 u64 request_id) noexcept {
    if (!rest || !request || !request_id || !soundboard_request_valid(*request) ||
        request->operation == SoundboardOperation::probe)
        return {RestClientSubmitError::invalid_input};
    u8 target[kSoundboardMaximumTargetBytes];
    const auto path = soundboard_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 reason[kRestAuditReasonMaximumEncodedBytes];
    RestAuditReasonResult encoded{0, true};
    const auto method = soundboard_method(request->operation);
    if (request->reason.bytes) {
        if (method == RestMethod::get || request->operation == SoundboardOperation::send)
            return {RestClientSubmitError::invalid_input};
        encoded = rest_encode_audit_reason(request->reason, {reason, sizeof(reason)});
        if (!encoded.ok)
            return {RestClientSubmitError::invalid_input};
    }
    RestClientSubmitResult result{};
    if (request->operation == SoundboardOperation::create)
        result = submit_create(rest, *request, {target, path.bytes}, {reason, encoded.bytes}, request_id);
    else {
        u8 body[kSoundboardMaximumSmallBodyBytes];
        SoundboardWriteResult written{};
        if (soundboard_has_body(request->operation)) {
            written = soundboard_write_body(*request, {body, sizeof(body)});
            if (!written.ok()) {
                secure_clear_bytes(reason, encoded.bytes);
                return {RestClientSubmitError::body_capacity};
            }
        }
        result = rest_client_submit(rest, {{target, path.bytes},
                                           written.bytes ? ConstSlice<u8>{body, written.bytes}
                                                                 : ConstSlice<u8>{},
                                           soundboard_route(*request),
                                           request_id,
                                           kRestClientDefaultTimeoutMs,
                                           written.bytes != 0,
                                           false,
                                           {reason, encoded.bytes}});
        secure_clear_bytes(body, written.bytes);
    }
    secure_clear_bytes(reason, encoded.bytes);
    return result;
}

namespace {
void render_preview(void* context, Slice<i16> pcm, u64 now_us) noexcept {
    auto* engine = static_cast<AccountSoundboard*>(context);
    i32 mix[kVoiceRenderMaximumFrames * 2];
    const u32 frames = minimum(pcm.count / 2, kVoiceRenderMaximumFrames);
    clear_bytes(pcm.data, pcm.count * static_cast<u32>(sizeof(i16)));
    clear_bytes(mix, frames * 2 * static_cast<u32>(sizeof(i32)));
    soundboard_mixer_render(&engine->mixer, {mix, frames * 2}, frames, engine->output_gain_percent, false, now_us);
    voice_mixer_finish({mix, frames * 2}, {pcm.data, frames * 2});
}

bool start_cdn(AccountSoundboard* s) noexcept {
    if (s->cdn_started)
        return true;
    if (!cdn_client_start(&s->cdn, kSoundboardCdnResponseBytes).ok()) {
        s->last_error = SoundboardAudioError::fetch;
        return false;
    }
    s->cdn_started = true;
    return true;
}

bool fetch(AccountSoundboard* s, u64 sound_id) noexcept {
    if (!start_cdn(s))
        return false;
    u8 target[kSoundboardCdnTargetBytes];
    const auto written = soundboard_write_cdn_target(sound_id, {target, sizeof(target)});
    if (!written.ok())
        return false;
    const u64 request_id = ++s->next_request_id;
    CdnClientRequest request{};
    request.target = {target, written.bytes};
    request.request_id = request_id;
    request.timeout_ms = kSoundboardCdnTimeoutMs;
    request.host = CdnHost::cdn;
    request.image_type = CdnImageType::soundboard_sound;
    if (!cdn_client_submit(&s->cdn, request).ok())
        return false;
    s->fetch_sound_id = sound_id;
    s->fetch_request_id = request_id;
    return true;
}

struct Consumed {
    AccountSoundboard* engine;
    SoundboardClip* clip;
    SoundboardDecodeStatus status;
};
bool consume(void* context, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
    auto& c = *static_cast<Consumed*>(context);
    if (response.request_id != c.engine->fetch_request_id || !response.transport_ok() ||
        cdn_client_response_policy(response).disposition != CdnResponseDisposition::accept_body) {
        c.status = SoundboardDecodeStatus::input;
        return true;
    }
    c.clip = soundboard_clip_decode(c.engine->fetch_sound_id, body, &c.status);
    return true;
}

void drop_pending(AccountSoundboard* s, u32 index) noexcept {
    s->pending[index] = s->pending[--s->pending_count];
    reset_value(&s->pending[s->pending_count]);
}

bool ensure_preview(AccountSoundboard* s, u64 now_ms) noexcept {
    s->preview_idle_since_ms = now_ms;
    if (s->call_audio || voice_audio_output_running(s->preview))
        return true;
    if (!voice_audio_output_start_device(&s->preview, s->output_device, render_preview, s)) {
        s->last_error = SoundboardAudioError::output;
        return false;
    }
    return true;
}

void start_ready(AccountSoundboard* s, u64 now_ms) noexcept {
    for (u32 i = 0; i < s->pending_count;) {
        auto& play = s->pending[i];
        if (now_ms > play.queued_ms + kSoundboardPendingTimeoutMs ||
            (play.sound_id == s->failed_sound_id && now_ms < s->failed_until_ms)) {
            drop_pending(s, i);
            continue;
        }
        if (auto* clip = soundboard_cache_find(&s->cache, play.sound_id)) {
            if (play.preview || !s->call_audio)
                ensure_preview(s, now_ms);
            soundboard_mixer_play(&s->mixer, clip, play.volume, play.user_id);
            drop_pending(s, i);
            continue;
        }
        ++i;
    }
    if (!s->fetch_request_id)
        for (u32 i = 0; i < s->pending_count; ++i)
            if (fetch(s, s->pending[i].sound_id))
                break;
}

void fill_status(AccountSoundboard* s, SoundboardAudioStatus* status) noexcept {
    if (!status)
        return;
    reset_value(status);
    status->fetching_sound_id = s->fetch_request_id ? s->fetch_sound_id : 0;
    status->failed_sound_id = s->failed_sound_id;
    status->active_voices = soundboard_mixer_active(&s->mixer);
    status->cached_clips = s->cache.count;
    status->pending = s->pending_count;
    status->last_error = s->last_error;
    status->call_audio = s->call_audio;
}
} // namespace

void account_soundboard_initialize(AccountSoundboard* s) noexcept {
    if (!s)
        return;
    clear_bytes(s, static_cast<u32>(sizeof(*s)));
    soundboard_cache_initialize(&s->cache);
    s->output_gain_percent = 100;
    s->initialized = true;
}

void account_soundboard_output(AccountSoundboard* s, const VoiceAudioSettings& settings) noexcept {
    if (!s || !s->initialized)
        return;
    s->output_gain_percent = minimum<u32>(settings.output_gain_percent, kVoiceGainMaximumPercent);
    if (voice_device_id_equal(settings.output_device, s->output_device))
        return;
    copy_bytes(s->output_device, settings.output_device, sizeof(s->output_device));
    s->output_device[kVoiceDeviceIdCharacters - 1] = 0;
    if (voice_audio_output_running(s->preview)) {
        voice_audio_output_stop(&s->preview);
        voice_audio_output_start_device(&s->preview, s->output_device, render_preview, s);
    }
}

bool account_soundboard_command(AccountSoundboard* s, GatewayClient* gateway, const SoundboardAudioCommand* command,
                                SoundboardAudioStatus* status, u64 now_ms) noexcept {
    if (!s || !s->initialized)
        return false;
    s->call_audio = gateway_client_attach_soundboard(gateway, &s->mixer);
    bool accepted = true;
    if (command) {
        switch (command->operation) {
        case SoundboardAudioOperation::status:
            break;
        case SoundboardAudioOperation::configure:
            s->muted = command->muted;
            soundboard_mixer_set_volume(&s->mixer, command->volume_percent);
            break;
        case SoundboardAudioOperation::stop:
            soundboard_mixer_stop(&s->mixer, command->user_id);
            for (u32 i = 0; i < s->pending_count;)
                if (!command->user_id || s->pending[i].user_id == command->user_id)
                    drop_pending(s, i);
                else
                    ++i;
            break;
        case SoundboardAudioOperation::play:
        case SoundboardAudioOperation::preview: {
            const bool preview = command->operation == SoundboardAudioOperation::preview;
            // Received sounds play only inside a call and never while muted.
            if (!command->sound_id || command->sound_volume > kSoundboardVolumeScale ||
                (!preview && (s->muted || !s->call_audio))) {
                accepted = false;
                break;
            }
            if (s->pending_count == kSoundboardPendingPlays)
                drop_pending(s, 0);
            s->pending[s->pending_count++] = {command->sound_id, command->user_id, now_ms, command->sound_volume,
                                              preview};
            if (command->sound_id == s->failed_sound_id && now_ms >= s->failed_until_ms)
                s->failed_sound_id = 0;
            start_ready(s, now_ms);
            break;
        }
        default:
            accepted = false;
            break;
        }
    }
    fill_status(s, status);
    return accepted;
}

void account_soundboard_pump(AccountSoundboard* s, GatewayClient* gateway, u64 now_ms) noexcept {
    if (!s || !s->initialized)
        return;
    s->call_audio = gateway_client_attach_soundboard(gateway, &s->mixer);
    if (s->cdn_started && s->fetch_request_id &&
        cdn_client_status(&s->cdn).phase == CdnClientPhase::response_ready) {
        Consumed consumed{s, nullptr, SoundboardDecodeStatus::none};
        cdn_client_consume_response(&s->cdn, consume, &consumed);
        if (consumed.clip)
            soundboard_cache_insert(&s->cache, consumed.clip);
        else {
            s->failed_sound_id = s->fetch_sound_id;
            s->failed_until_ms = now_ms + kSoundboardFailureHoldMs;
            s->last_error = consumed.status == SoundboardDecodeStatus::memory ? SoundboardAudioError::memory
                            : consumed.status == SoundboardDecodeStatus::input ? SoundboardAudioError::fetch
                                                                               : SoundboardAudioError::decode;
        }
        s->fetch_request_id = 0;
        s->fetch_sound_id = 0;
    } else if (s->cdn_started && s->fetch_request_id &&
               cdn_client_status(&s->cdn).phase == CdnClientPhase::failed) {
        // A failed worker cannot recover; restart it on the next fetch.
        cdn_client_stop(&s->cdn, 1'000);
        s->cdn_started = s->cdn.worker.thread != nullptr;
        s->failed_sound_id = s->fetch_sound_id;
        s->failed_until_ms = now_ms + kSoundboardFailureHoldMs;
        s->last_error = SoundboardAudioError::fetch;
        s->fetch_request_id = s->fetch_sound_id = 0;
    }
    start_ready(s, now_ms);
    if (voice_audio_output_running(s->preview)) {
        const u64 now_us = voice_playback_now_us();
        if (soundboard_mixer_active(&s->mixer) && !soundboard_mixer_call_rendering(&s->mixer, now_us))
            s->preview_idle_since_ms = now_ms;
        else if (now_ms >= s->preview_idle_since_ms + kSoundboardPreviewIdleMs)
            voice_audio_output_stop(&s->preview);
    }
}

HANDLE account_soundboard_notification_handle(const AccountSoundboard* s) noexcept {
    return s && s->cdn_started && s->fetch_request_id ? cdn_client_notification_handle(&s->cdn) : nullptr;
}

bool account_soundboard_stop(AccountSoundboard* s, GatewayClient* gateway, u32 timeout_ms) noexcept {
    if (!s || !s->initialized)
        return true;
    gateway_client_attach_soundboard(gateway, nullptr);
    voice_audio_output_stop(&s->preview);
    soundboard_mixer_stop(&s->mixer, 0);
    if (s->cdn_started) {
        cdn_client_stop(&s->cdn, timeout_ms);
        if (s->cdn.worker.thread)
            return false; // Not joined: the worker still owns its storage.
        s->cdn_started = false;
    }
    soundboard_cache_clear(&s->cache);
    s->pending_count = 0;
    s->fetch_request_id = 0;
    return true;
}
} // namespace maicord

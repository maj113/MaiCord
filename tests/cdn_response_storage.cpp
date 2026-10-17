// Exercise the shipping private response owner without a network or test-only API.
#include "../src/app/cdn_client.cpp"

namespace maicord {
void test_cdn_response_storage(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr char audio_head[] = "HTTP/1.1 200 OK\r\nContent-Length: 120\r\nContent-Type: audio/ogg\r\n\r\n";
    constexpr char codec_head[] = "HTTP/1.1 200 OK\r\nContent-Length: 120\r\nContent-Type: audio/ogg; codecs=opus\r\n\r\n";
    const auto audio_policy = validate_static_image_response(CdnImageType::ogg_opus,
        {reinterpret_cast<const u8*>(audio_head), sizeof(audio_head)-1}, 120, 1);
    check(audio_policy.ok() &&
              validate_static_image_response(CdnImageType::ogg_opus,
                  {reinterpret_cast<const u8*>(codec_head), sizeof(codec_head)-1}, 120, 1).ok() &&
              validate_static_image_response(CdnImageType::mp4,
                  {reinterpret_cast<const u8*>(audio_head), sizeof(audio_head)-1}, 120, 1).error ==
                  CdnHttpError::invalid_content_type &&
              validate_static_image_response(CdnImageType::ogg_opus,
                  {reinterpret_cast<const u8*>(audio_head), sizeof(audio_head)-1},
                  kCdnMaximumEncodedBodyBytes+1, 1).error == CdnHttpError::body_too_large,
          "cdn_audio_typed_ogg_mime_and_body_bounds_leave_video_policy_distinct");
    // The image body only needs to exceed the 64 KiB reusable tail; the video body must exceed the 16 MiB
    // image limit to prove the larger reservation streams.
    constexpr u32 body_sizes[]{256u * 1024u, 18'632'225u};
    constexpr char small_head[] = "HTTP/1.1 200 OK\r\nContent-Length: 262144\r\n\r\n";
    constexpr char video_head[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 18632225\r\nContent-Type: video/mp4\r\n\r\n";
    constexpr char oversized_head[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 67108865\r\nContent-Type: video/mp4\r\n\r\n";
    const ConstSlice<u8> actual_video_head{reinterpret_cast<const u8*>(video_head), sizeof(video_head) - 1};
    check(validate_static_image_response(CdnImageType::mp4, actual_video_head, body_sizes[1],
                                         0)
                  .ok() &&
              validate_static_image_response(CdnImageType::png, actual_video_head, 0, 0)
                      .error == CdnHttpError::body_too_large &&
              validate_static_image_response(
                  CdnImageType::mp4,
                  {reinterpret_cast<const u8*>(oversized_head), sizeof(oversized_head) - 1}, 0, 0)
                      .error == CdnHttpError::body_too_large,
          "cdn_video_body_policy_accepts_actual_18mb_header_and_rejects_image_and_over64mb_before_body");
    constexpr CdnImageType image_types[]{CdnImageType::png, CdnImageType::webp,
                                         CdnImageType::gif, CdnImageType::jpeg};
    bool image_caps = true;
    for (const auto type : image_types)
        image_caps =
            image_caps && validate_static_image_response(type, actual_video_head, 0, 0).error ==
                              CdnHttpError::body_too_large;
    check(image_caps, "cdn_video_larger_reservation_does_not_expand_ordinary_image_header_admission");
    CdnClient client{};
    auto* runtime = static_cast<CdnClientRuntime*>(VirtualAlloc(
        nullptr, allocation_bytes(kMediaMaximumEncodedVideoBytes), MEM_RESERVE, PAGE_READWRITE));
    if (!runtime) {
        check(false, "cdn_response_storage_reserve");
        return;
    }
    const u32 initial = (sizeof(CdnClientRuntime) + fixed_storage_bytes() + 4095u) & ~4095u;
    if (!VirtualAlloc(runtime, initial, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(runtime, 0, MEM_RELEASE);
        check(false, "cdn_response_storage_initial_commit");
        return;
    }
    runtime->allocation_bytes = allocation_bytes(kMediaMaximumEncodedVideoBytes);
    runtime->storage_committed_bytes = initial;
    runtime->response_capacity = kMediaMaximumEncodedVideoBytes;
    assign_storage(runtime);
    client.runtime = runtime;
    client.response_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool valid = client.response_event != nullptr;
    u8 chunk[16 * 1024];
    for (auto& byte : chunk)
        byte = 0xA5;
    u64 retained = 0;
    for (u32 round = 0; valid && round < 2; ++round) {
        u32 body_bytes = body_sizes[round];
        valid = http_response_decoder_initialize(
            &runtime->response, {runtime->response_input, kResponseInputBytes},
            {runtime->response_head, kCdnMaximumResponseHeaderBytes},
            {runtime->response_body,
             minimum(runtime->response_capacity,
                     cdn_encoded_body_limit(round ? CdnImageType::mp4
                                                          : CdnImageType::png))},
            kCdnMaximumResponseHeaderBytes, kCdnMaximumResponseFields);
        valid = valid && response_plaintext(runtime,
                                            round ? actual_video_head
                                                  : ConstSlice<u8>{reinterpret_cast<const u8*>(small_head),
                                                                   sizeof(small_head) - 1},
                                            false) == TlsStreamReadAction::continue_read;
        for (u32 offset = 0; valid && offset < body_bytes; offset += sizeof(chunk)) {
            const u32 part = minimum(static_cast<u32>(sizeof(chunk)), body_bytes - offset);
            const auto action = response_plaintext(runtime, {chunk, part}, false);
            valid = action == (offset + part == body_bytes ? TlsStreamReadAction::complete
                                                           : TlsStreamReadAction::continue_read);
        }
        runtime->completed.body_bytes = body_bytes;
        client.request_state = request_state_ready;
        const auto rejected = cdn_client_consume_response(
            &client, [](void*, const CdnClientResponse&, ConstSlice<u8>) noexcept { return false; },
            nullptr);
        MEMORY_BASIC_INFORMATION before{};
        VirtualQuery(runtime->response_body + body_bytes - 1, &before, sizeof(before));
        valid = valid && rejected.error == CdnClientTakeError::consumer_rejected &&
                before.State == MEM_COMMIT && client.request_state == request_state_ready;
        const auto consumed = cdn_client_consume_response(
            &client,
            [](void* context, const CdnClientResponse&, ConstSlice<u8> body) noexcept {
                return body.count == *static_cast<const u32*>(context) && body[0] == 0xA5 &&
                       body[body.count - 1] == 0xA5;
            },
            &body_bytes);
        MEMORY_BASIC_INFORMATION after{};
        VirtualQuery(runtime->response_body + body_bytes - 1, &after, sizeof(after));
        retained = runtime->storage_committed_bytes;
        valid = valid && consumed.ok() && after.State == MEM_RESERVE && retained < initial + 128u * 1024u &&
                client.request_state == request_state_empty &&
                runtime->response_capacity == kMediaMaximumEncodedVideoBytes;
    }
    check(valid, "cdn_response_owner_releases_large_body_and_recommits_next_response_exactly");
    if (client.response_event)
        CloseHandle(client.response_event);
    VirtualFree(runtime, 0, MEM_RELEASE);
}
} // namespace maicord

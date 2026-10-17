#include "maicord/app/account_media_worker.hpp"

namespace maicord {
namespace {
// Original minimal Ogg/Opus container: 312 pre-skip and 100 end trim leave
// 548 audible frames. Its one-byte TOC packet is a valid empty CELT frame.
u32 audio_fixture(u8* bytes) noexcept {
    u8 head[19]{};
    copy_bytes(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 2;
    head[10] = 56;
    head[11] = 1;
    u8 tags[16]{};
    copy_bytes(tags, "OpusTags", 8);
    const u8 packet = 0xf8;
    const ConstSlice<u8> bodies[]{{head, 19}, {tags, 16}, {&packet, 1}};
    u32 count = 0;
    for (u32 page = 0; page < 3; ++page) {
        auto* p = bytes + count;
        const u32 size = 28 + bodies[page].count;
        ZeroMemory(p, size);
        copy_bytes(p, "OggS", 4);
        p[5] = page == 0 ? 2 : page == 2 ? 4 : 0;
        if (page == 2) {
            p[6] = 92;
            p[7] = 3;
        }
        p[14] = 23;
        p[18] = static_cast<u8>(page);
        p[26] = 1;
        p[27] = static_cast<u8>(bodies[page].count);
        copy_bytes(p + 28, bodies[page].data, bodies[page].count);
        u32 crc = 0;
        for (u32 i = 0; i < size; ++i) {
            crc ^= static_cast<u32>(p[i]) << 24;
            for (u32 bit = 0; bit < 8; ++bit)
                crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
        }
        for (u32 i = 0; i < 4; ++i)
            p[22 + i] = static_cast<u8>(crc >> (i * 8));
        count += size;
    }
    return count;
}
void deferred_cache_checks(void (*check)(bool, const char*) noexcept, bool indexed) noexcept {
    EncodedMediaCacheEntry entries[2]{};
    u16 owners[2]{};
    u8 storage[2 * kEncodedMediaCachePageBytes]{};
    u32 buckets[4]{};
    EncodedMediaCache cache{};
    u8 bytes[120]{};
    const u32 count = audio_fixture(bytes);
    const auto key = media_asset_attachment(100, MediaAttachmentFormat::ogg_opus);
    const auto other = media_asset_attachment(101, MediaAttachmentFormat::ogg_opus);
    const auto replacement = media_asset_attachment(102, MediaAttachmentFormat::ogg_opus);
    const EncodedMediaCachePolicy policy{{}, 200, true, false};
    check(encoded_media_cache_initialize(&cache, {entries, 2}, {owners, 2}, {storage, sizeof(storage)},
                                         indexed ? Slice<u32>{buckets, 4} : Slice<u32>{}),
          "retired cache indexed or linear initialize");
    const auto stored = encoded_media_cache_store(&cache, key, {bytes, count}, policy);
    auto first = encoded_media_cache_acquire(&cache, key, 1);
    auto second = encoded_media_cache_acquire(&cache, key, 1);
    auto old_token = first;
    const u64 identity = first.lookup.content_identity;
    check(stored.stored() && first.acquired && second.acquired &&
              !encoded_media_cache_retire(&cache, key, static_cast<u16>(stored.generation + 1)) &&
              encoded_media_cache_find(&cache, key, 1).found() &&
              encoded_media_cache_retire(&cache, key, stored.generation) &&
              encoded_media_cache_retire(&cache, key, stored.generation) &&
              !encoded_media_cache_find(&cache, key, 1).found() &&
              !encoded_media_cache_acquire(&cache, key, 1).acquired &&
              !encoded_media_cache_refresh(&cache, key, 300, false) &&
              !encoded_media_cache_store(&cache, key, {bytes, count}, policy).stored() &&
              cache.entry_count == 1 && cache.used_pages == 1 && cache.stored_bytes == count,
          "retirement immediately hides pinned source and rejects new same-key readers refresh and "
          "replacement");
    check(encoded_media_cache_store(&cache, other, {bytes, count}, policy).stored() &&
              encoded_media_cache_find(&cache, other, 1).content_identity != identity &&
              encoded_media_cache_store(&cache, replacement, {bytes, count}, policy).stored() &&
              bytes_equal(first.lookup.bytes.data, bytes, count) &&
              bytes_equal(second.lookup.bytes.data, bytes, count),
          "retired source is excluded from exact-content reuse and remains immutable under unrelated "
          "eviction");
    check(encoded_media_cache_release(&cache, &first) && cache.entry_count == 2 && cache.used_pages == 2 &&
              bytes_equal(second.lookup.bytes.data, bytes, count) &&
              encoded_media_cache_release(&cache, &second) && cache.entry_count == 1 &&
              cache.used_pages == 1 && cache.stored_bytes == count && cache.counters.erases == 1,
          "retirement final lease frees exactly its own pages and accounting once");
    const auto again = encoded_media_cache_store(&cache, key, {bytes, count}, policy);
    auto current = encoded_media_cache_acquire(&cache, key, 1);
    check(again.stored() && current.acquired && again.generation != old_token.generation &&
              !encoded_media_cache_release(&cache, &old_token) &&
              !encoded_media_cache_retire(&cache, key, stored.generation) &&
              !encoded_media_cache_erase(&cache, key) && current.lookup.bytes.count == count &&
              encoded_media_cache_release(&cache, &current),
          "retired lifetime cannot release or invalidate a newly stored generation");
    encoded_media_cache_erase(&cache, key);
    encoded_media_cache_erase(&cache, replacement);
    check(!cache.entry_count && !cache.used_pages && !cache.stored_bytes,
          "retired cache leaves no pages or retained sources after cleanup");
}
void retained_audio_checks(void (*check)(bool, const char*) noexcept) noexcept {
    EncodedMediaCacheEntry entries[2]{};
    u16 owners[2]{};
    u8 storage[2 * kEncodedMediaCachePageBytes]{};
    EncodedMediaCache cache{};
    u8 bytes[120]{};
    const u32 count = audio_fixture(bytes);
    const auto key = media_asset_attachment(9876, MediaAttachmentFormat::ogg_opus);
    const u8 etag[]{'"', 'a', '"'};
    check(count == 120 &&
              encoded_media_cache_initialize(&cache, {entries, 2}, {owners, 2}, {storage, sizeof(storage)}),
          "audio fixture and bounded shared cache initialize");
    CdnMediaPipeline pipeline{};
    pipeline.cache = &cache;
    CdnResponseResult response{};
    response.status_code = 200;
    response.body_bytes = count;
    response.store_allowed = true;
    response.expires_at_seconds = 100;
    response.etag = {etag, sizeof(etag)};
    response.disposition = CdnResponseDisposition::accept_body;
    auto admitted = cdn_media_pipeline_admit_audio(&pipeline, key, response, {bytes, count});
    const u64 identity = admitted.lease.lookup.content_identity;
    const u16 generation = admitted.lease.generation;
    check(admitted.ready() && admitted.lease.acquired && identity &&
              encoded_audio_total_frames(admitted.metadata) == 548 &&
              encoded_audio_bytes(admitted.metadata).data != bytes &&
              !encoded_media_cache_erase(&cache, key),
          "audio validated source owns cache lease and exact audible granules");
    if (admitted.lease.acquired)
        encoded_media_cache_release(&cache, &admitted.lease);
    u32 cancellations = 0;
    const OggOpusCancelled cancel = +[](void* context) noexcept {
        ++*static_cast<u32*>(context);
        return true;
    };
    auto cancelled_hit = cdn_media_pipeline_lookup_audio(&pipeline, key, 1, cancel, &cancellations);
    auto cancelled_body = cdn_media_pipeline_admit_audio(&pipeline, key, response, {bytes, count}, {},
                                                         cancel, &cancellations);
    check(cancelled_hit.error == EncodedAudioError::cancelled && !cancelled_hit.lease.acquired &&
              cancelled_body.error == EncodedAudioError::cancelled && !cancelled_body.lease.acquired &&
              cancellations == 2 && encoded_media_cache_find(&cache, key, 1).content_identity == identity,
          "audio full validation observes worker cancellation and releases parse lease without erasing "
          "valid cache");
    auto stale = cdn_media_pipeline_lookup_audio(&pipeline, key, 101);
    check(stale.state == CdnEncodedAudioState::fetch_required && stale.etag.count == sizeof(etag) &&
              !stale.lease.acquired,
          "audio stale entry copies conditional metadata without a source lease");
    response.status_code = 304;
    response.body_bytes = 0;
    response.expires_at_seconds = 200;
    response.disposition = CdnResponseDisposition::not_modified;
    const u8 wrong[]{'"', 'b', '"'};
    auto rejected = cdn_media_pipeline_admit_audio(&pipeline, key, response, {}, {wrong, sizeof(wrong)});
    check(rejected.state == CdnEncodedAudioState::uncorrelated_not_modified && !rejected.lease.acquired,
          "audio 304 rejects different ETag without publishing or retaining bytes");
    auto refreshed = cdn_media_pipeline_admit_audio(&pipeline, key, response, {}, {etag, sizeof(etag)});
    check(refreshed.ready() && refreshed.lease.lookup.content_identity == identity &&
              refreshed.lease.generation == generation &&
              encoded_audio_total_frames(refreshed.metadata) == 548,
          "audio correlated 304 preserves immutable source identity and generation");
    if (refreshed.lease.acquired)
        encoded_media_cache_release(&cache, &refreshed.lease);
    check(encoded_media_cache_find(&cache, key, 150).state == EncodedMediaCacheLookupState::fresh,
          "audio 304 refresh happens after releasing parse lease");
    AccountMedia media{};
    media.started = true;
    media.pipeline.cache = &cache;
    u8 scanline{};
    media.pipeline.scanline_storage = &scanline;
    AccountMediaAttachmentRequest request{};
    request.key = key;
    request.source_message_id = 777;
    request.source_message_revision = 2;
    request.image_type = CdnImageType::ogg_opus;
    request.signed_url = utf8_literal("https://cdn.discordapp.com/attachments/7/9876/voice.ogg");
    const auto ready = account_media_request_attachment(&media, request, 1, 150);
    AccountMediaFrame frame{};
    check(ready.state == AccountMediaRequestState::ready && !media.client_started &&
              account_media_take_frame(&media, &frame) &&
              frame.kind == AccountMediaFrameKind::audio_source && frame.content_identity == identity &&
              frame.cache_generation == generation && frame.source_message_id == 777 &&
              frame.source_message_revision == 2 && frame.duration_us == 11416 && !frame.width &&
              !frame.height && !frame.bytes.count && !frame.gpu_video && !media.mp4_decoder &&
              account_media_audio_matches(&media, key, generation) &&
              !account_media_audio_matches(&media, key, static_cast<u16>(generation + 1)) &&
              !encoded_media_cache_erase(&cache, key),
          "audio cache hit publishes metadata only without network video decoder or GPU and keeps source "
          "pinned");
    account_media_release_audio_source(&media);
    check(!account_media_audio_matches(&media, key, generation) && encoded_media_cache_erase(&cache, key),
          "audio joined playback release retires source and unpins cache");
    response.status_code = 200;
    response.body_bytes = count;
    response.store_allowed = false;
    response.disposition = CdnResponseDisposition::accept_body;
    auto transient = cdn_media_pipeline_admit_audio(&pipeline, key, response, {bytes, count});
    check(transient.ready() && transient.state == CdnEncodedAudioState::ready_transient &&
              !transient.lease.acquired && encoded_audio_bytes(transient.metadata).data == bytes &&
              !encoded_media_cache_find(&cache, key, 0).found(),
          "audio no-store response requires lane-owned copy and never publishes borrowed cache bytes");
    response.store_allowed = true;
    auto stored_again = cdn_media_pipeline_admit_audio(&pipeline, key, response, {bytes, count});
    if (stored_again.lease.acquired)
        encoded_media_cache_release(&cache, &stored_again.lease);
    response.store_allowed = false;
    response.status_code = 304;
    response.body_bytes = 0;
    response.disposition = CdnResponseDisposition::not_modified;
    auto other_reader = encoded_media_cache_acquire(&cache, key, 0);
    auto no_store_304 =
        cdn_media_pipeline_admit_audio(&pipeline, key, response, {}, {etag, sizeof(etag)});
    check(no_store_304.state == CdnEncodedAudioState::ready_transient && no_store_304.lease.acquired &&
              encoded_audio_bytes(no_store_304.metadata).data != bytes &&
              !encoded_media_cache_erase(&cache, key) &&
              !encoded_media_cache_find(&cache, key, 0).found() &&
              !encoded_media_cache_acquire(&cache, key, 0).acquired,
          "audio no-store 304 pins bytes for existing readers but immediately forbids new cache reuse");
    if (no_store_304.lease.acquired)
        encoded_media_cache_release(&cache, &no_store_304.lease);
    check(other_reader.acquired && cache.entry_count == 1 &&
              bytes_equal(other_reader.lookup.bytes.data, bytes, count) &&
              encoded_media_cache_release(&cache, &other_reader) && !cache.entry_count && !cache.used_pages,
          "audio no-store 304 final independent reader retires bytes without another erase call");
    response.status_code = 200;
    response.body_bytes = count;
    response.disposition = CdnResponseDisposition::accept_body;
    bytes[119] ^= 1;
    auto invalid = cdn_media_pipeline_admit_audio(&pipeline, key, response, {bytes, count});
    check(invalid.state == CdnEncodedAudioState::decode_rejected &&
              !encoded_media_cache_find(&cache, key, 0).found(),
          "audio bad Ogg CRC fails before cache admission");
}
void common_audio_checks(void (*check)(bool, const char*) noexcept) noexcept {
    const auto opus_key = media_asset_attachment(710, MediaAttachmentFormat::ogg_opus);
    const auto mp3_key =
        media_asset_attachment(710, MediaAttachmentFormat::mp3, MediaResolutionTier::px2048);
    const auto wave_key =
        media_asset_attachment(710, MediaAttachmentFormat::wav, MediaResolutionTier::px2048);
    const auto png_key = media_asset_attachment(710, MediaAttachmentFormat::png);
    const auto search_wave = media_asset_search_attachment(
        710, MediaAttachmentFormat::wav, MediaResolutionTier::px2048, kMediaSearchRevisionMaximum, true);
    auto bad_tier = wave_key;
    bad_tier.variant |= 1u << kMediaAttachmentResolutionShift;
    auto bad_animation = mp3_key;
    bad_animation.flags |= media_asset_flag_animated;
    check(opus_key.variant == 7 && !opus_key.flags && media_asset_key_valid(mp3_key) &&
              media_asset_key_valid(wave_key) && media_asset_key_valid(search_wave) &&
              media_asset_attachment_format(mp3_key) == MediaAttachmentFormat::mp3 &&
              media_asset_attachment_format(wave_key) == MediaAttachmentFormat::wav &&
              media_asset_attachment_format(search_wave) == MediaAttachmentFormat::wav &&
              media_asset_search_revision(search_wave) == kMediaSearchRevisionMaximum &&
              media_asset_search_snapshot(search_wave) &&
              media_asset_resolution_tier(mp3_key) == MediaResolutionTier::original &&
              !media_asset_key_equal(mp3_key, opus_key) && !media_asset_key_equal(wave_key, png_key) &&
              !media_asset_key_valid(bad_tier) && !media_asset_key_valid(bad_animation) &&
              !media_asset_key_valid(media_asset_attachment(710, static_cast<MediaAttachmentFormat>(17))),
          "audio keys preserve legacy bits and full Search publication while separating MP3 WAV image and "
          "invalid formats");

    u8 wave[48]{};
    const auto put32 = [&wave](u32 offset, u32 value) noexcept {
        for (u32 i = 0; i < 4; ++i)
            wave[offset + i] = static_cast<u8>(value >> (i * 8));
    };
    copy_bytes(wave, "RIFF", 4);
    put32(4, 40);
    copy_bytes(wave + 8, "WAVEfmt ", 8);
    put32(16, 16);
    wave[20] = 1;
    wave[22] = 1;
    put32(24, 8000);
    put32(28, 16000);
    wave[32] = 2;
    wave[34] = 16;
    copy_bytes(wave + 36, "data", 4);
    put32(40, 4);
    wave[45] = 0x80;
    wave[46] = 0xff;
    wave[47] = 0x7f;
    EncodedMediaCacheEntry entries[4]{};
    u16 owners[4]{};
    u8 storage[4 * kEncodedMediaCachePageBytes]{};
    EncodedMediaCache cache{};
    check(encoded_media_cache_initialize(&cache, {entries, 4}, {owners, 4}, {storage, sizeof(storage)}),
          "common audio bounded encoded cache initializes");
    CdnMediaPipeline pipeline{};
    pipeline.cache = &cache;
    const u8 etag[]{'"', 'w', '"'};
    CdnResponseResult response{};
    response.status_code = 200;
    response.body_bytes = sizeof(wave);
    response.store_allowed = true;
    response.expires_at_seconds = 100;
    response.etag = {etag, sizeof(etag)};
    response.disposition = CdnResponseDisposition::accept_body;
    auto admitted = cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {wave, sizeof(wave)});
    const u64 identity = admitted.lease.lookup.content_identity;
    check(admitted.ready() && admitted.metadata.format == MediaAttachmentFormat::wav &&
              admitted.lease.acquired && identity && encoded_audio_total_frames(admitted.metadata) == 2 &&
              encoded_audio_channels(admitted.metadata) == 1 &&
              encoded_audio_rate(admitted.metadata) == 8000 &&
              admitted.metadata.wave.data.data == admitted.lease.lookup.bytes.data + 44 &&
              admitted.metadata.wave.data.count == 4 && !encoded_media_cache_erase(&cache, wave_key),
          "WAV admission validates metadata and rebases encoded and PCM subrange into one immutable lease");
    const u64 copied = cache.counters.bytes_copied;
    auto hit = cdn_media_pipeline_lookup_audio(&pipeline, wave_key, 1);
    check(hit.ready() && hit.lease.acquired && hit.lease.lookup.content_identity == identity &&
              encoded_audio_bytes(hit.metadata).data == encoded_audio_bytes(admitted.metadata).data &&
              cache.counters.bytes_copied == copied,
          "WAV cache read transfers same immutable source without an encoded body copy");
    if (hit.lease.acquired)
        encoded_media_cache_release(&cache, &hit.lease);
    if (admitted.lease.acquired)
        encoded_media_cache_release(&cache, &admitted.lease);

    const EncodedMediaCachePolicy policy{{}, 100, true, false};
    const auto shared = encoded_media_cache_store(&cache, search_wave, {wave, sizeof(wave)}, policy);
    const auto other_format = encoded_media_cache_store(&cache, mp3_key, {wave, sizeof(wave)}, policy);
    check(shared.stored() && other_format.stored() &&
              encoded_media_cache_find(&cache, search_wave, 1).content_identity == identity &&
              encoded_media_cache_find(&cache, mp3_key, 1).content_identity != identity,
          "identical encoded bytes share timeline Search identity only within the same audio format");
    encoded_media_cache_erase(&cache, mp3_key);
    auto wrong_format = cdn_media_pipeline_admit_audio(&pipeline, mp3_key, response, {wave, sizeof(wave)});
    check(wrong_format.state == CdnEncodedAudioState::decode_rejected && !wrong_format.lease.acquired &&
              !encoded_media_cache_find(&cache, mp3_key, 1).found(),
          "WAV body cannot enter MP3 metadata or encoded cache admission");
    encoded_media_cache_store(&cache, mp3_key, {wave, sizeof(wave)}, policy);
    auto bad_reader = encoded_media_cache_acquire(&cache, mp3_key, 1);
    auto bad_hit = cdn_media_pipeline_lookup_audio(&pipeline, mp3_key, 1);
    check(bad_reader.acquired && bad_hit.state == CdnEncodedAudioState::decode_rejected &&
              !bad_hit.lease.acquired && !encoded_media_cache_find(&cache, mp3_key, 1).found() &&
              !encoded_media_cache_acquire(&cache, mp3_key, 1).acquired &&
              bytes_equal(bad_reader.lookup.bytes.data, wave, sizeof(wave)) &&
              encoded_media_cache_release(&cache, &bad_reader),
          "invalid cached audio retires exact source before release while an existing reader remains "
          "immutable");

    response.store_allowed = false;
    auto transient = cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {wave, sizeof(wave)});
    u8 copy[48]{};
    copy_bytes(copy, wave, sizeof(wave));
    const auto original = transient.metadata;
    check(
        transient.ready() && !transient.lease.acquired &&
            !encoded_audio_rebind(&transient.metadata, {copy, sizeof(copy) - 1}) &&
            encoded_audio_bytes(transient.metadata).data == encoded_audio_bytes(original).data &&
            encoded_audio_rebind(&transient.metadata, {copy, sizeof(copy)}) &&
            encoded_audio_bytes(transient.metadata).data == copy &&
            transient.metadata.wave.data.data == copy + 44 &&
            !encoded_media_cache_find(&cache, wave_key, 1).found(),
        "transient WAV requires one owned copy and atomic exact-size subrange rebind without cache reuse");
    auto invalid_range = transient.metadata;
    invalid_range.wave.data = {copy + sizeof(copy), 4};
    check(!encoded_audio_rebind(&invalid_range, {wave, sizeof(wave)}) &&
              encoded_audio_bytes(invalid_range).data == copy &&
              invalid_range.wave.data.data == copy + sizeof(copy),
          "malformed WAV subrange rebind rejects atomically without changing encoded source");

    response.store_allowed = true;
    auto prior = cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {wave, sizeof(wave)});
    const u16 prior_generation = prior.lease.generation;
    if (prior.lease.acquired)
        encoded_media_cache_release(&cache, &prior.lease);
    const u8 replacement_etag[]{'"', 'n', '"'};
    copy[44] = 23;
    response.etag = {replacement_etag, sizeof(replacement_etag)};
    response.expires_at_seconds = 300;
    auto replacement = cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {copy, sizeof(copy)});
    const u16 replacement_generation = replacement.lease.generation;
    const u64 replacement_identity = replacement.lease.lookup.content_identity;
    const bool replacement_acquired = replacement.lease.acquired;
    if (replacement.lease.acquired)
        encoded_media_cache_release(&cache, &replacement.lease);
    response.status_code = 304;
    response.body_bytes = 0;
    response.disposition = CdnResponseDisposition::not_modified;
    response.expires_at_seconds = 900;
    response.etag = {etag, sizeof(etag)};
    auto obsolete = cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {}, {etag, sizeof(etag)});
    const bool obsolete_refresh =
        encoded_media_cache_refresh(&cache, wave_key, 900, false, {etag, sizeof(etag)}, prior_generation);
    const bool obsolete_retire = encoded_media_cache_retire(&cache, wave_key, prior_generation);
    const auto current = encoded_media_cache_find(&cache, wave_key, 299);
    check(prior.ready() && replacement.ready() && replacement_acquired &&
              replacement_generation != prior_generation && !obsolete_refresh && !obsolete_retire &&
              obsolete.state == CdnEncodedAudioState::uncorrelated_not_modified &&
              !obsolete.lease.acquired && !encoded_audio_ready(obsolete.metadata) &&
              current.state == EncodedMediaCacheLookupState::fresh && current.expires_at_seconds == 300 &&
              current.etag.count == sizeof(replacement_etag) &&
              bytes_equal(current.etag.data, replacement_etag, sizeof(replacement_etag)) &&
              current.bytes.count == sizeof(copy) && bytes_equal(current.bytes.data, copy, sizeof(copy)),
          "obsolete audio 304 generation cannot refresh retire or rebind replacement bytes ETag and "
          "freshness");
    response.status_code = 200;
    response.body_bytes = sizeof(copy);
    response.disposition = CdnResponseDisposition::accept_body;
    u8 malformed[48]{};
    copy_bytes(malformed, copy, sizeof(copy));
    malformed[0] = 'X';
    auto rejected_body =
        cdn_media_pipeline_admit_audio(&pipeline, wave_key, response, {malformed, sizeof(malformed)});
    check(rejected_body.state == CdnEncodedAudioState::decode_rejected &&
              encoded_media_cache_find(&cache, wave_key, 299).content_identity == replacement_identity,
          "rejected new audio body preserves unrelated validated cache lifetime");
    encoded_media_cache_erase(&cache, wave_key);

    static constexpr char mp3_head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 48\r\n\r\n";
    static constexpr char wave_head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/x-wav\r\nContent-Length: 48\r\n\r\n";
    const ConstSlice<u8> mp3_http{reinterpret_cast<const u8*>(mp3_head), sizeof(mp3_head) - 1};
    const ConstSlice<u8> wave_http{reinterpret_cast<const u8*>(wave_head), sizeof(wave_head) - 1};
    check(
        validate_static_image_response(CdnImageType::mp3, mp3_http, 48, 1).ok() &&
            validate_static_image_response(CdnImageType::wav, wave_http, 48, 1).ok() &&
            validate_static_image_response(CdnImageType::wav, mp3_http, 48, 1).error ==
                CdnHttpError::invalid_content_type &&
            validate_static_image_response(CdnImageType::mp3, mp3_http, kCdnMaximumEncodedBodyBytes + 1, 1)
                    .error == CdnHttpError::body_too_large,
        "typed MP3 WAV HTTP admission accepts supported MIME and rejects mismatched formats and "
        "oversized bodies");
    const auto validate_head = [](CdnImageType type, Utf8View header) noexcept {
        return validate_static_image_response(
            type, {reinterpret_cast<const u8*>(header.data), header.bytes}, 48, 1);
    };
    const auto parameterized_mp3 = utf8_literal(
        "HTTP/1.1 200 OK\r\nContent-Type: \tAuDiO/MpEg \t; charset=binary\r\nContent-Length: 48\r\n\r\n");
    const auto parameterized_wave = utf8_literal(
        "HTTP/1.1 200 OK\r\nContent-Type: AUDIO/X-WAV; codec=pcm\r\nContent-Length: 48\r\n\r\n");
    check(validate_head(CdnImageType::mp3, parameterized_mp3).ok() &&
              validate_head(CdnImageType::wav, parameterized_wave).ok() &&
              validate_head(CdnImageType::mp3, parameterized_wave).error ==
                  CdnHttpError::invalid_content_type,
          "MP3 WAV HTTP media prefixes normalize case parameters and OWS but reject wrong specific audio "
          "type");
    const auto generic_head =
        utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream; "
                     "charset=binary\r\nContent-Length: 48\r\nCache-Control: max-age=100\r\n\r\n");
    const auto binary_head = utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: BINARY/OCTET-STREAM \t; "
                                          "ignored=value\r\nContent-Length: 48\r\n\r\n");
    const auto generic_wave_http = validate_head(CdnImageType::wav, generic_head);
    auto generic_wave =
        cdn_media_pipeline_admit_audio(&pipeline, wave_key, generic_wave_http, {wave, sizeof(wave)});
    const auto generic_mp3_http = validate_head(CdnImageType::mp3, generic_head);
    auto mismatched_payload =
        cdn_media_pipeline_admit_audio(&pipeline, mp3_key, generic_mp3_http, {wave, sizeof(wave)});
    check(generic_wave_http.ok() && generic_mp3_http.ok() &&
              validate_head(CdnImageType::mp3, binary_head).ok() &&
              validate_head(CdnImageType::wav, binary_head).ok() && generic_wave.ready() &&
              generic_wave.lease.acquired && generic_wave.metadata.format == MediaAttachmentFormat::wav &&
              generic_wave.metadata.wave.data.data == generic_wave.lease.lookup.bytes.data + 44 &&
              mismatched_payload.state == CdnEncodedAudioState::decode_rejected &&
              !mismatched_payload.lease.acquired && !encoded_media_cache_find(&cache, mp3_key, 1).found(),
          "generic audio MIME permits exact WAV parsing and retained lease but never admits a WAV body as "
          "MP3");
    if (generic_wave.lease.acquired)
        encoded_media_cache_release(&cache, &generic_wave.lease);
    encoded_media_cache_erase(&cache, wave_key);
    const auto missing_type = utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 48\r\n\r\n");
    const auto empty_type =
        utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: \t\r\nContent-Length: 48\r\n\r\n");
    const auto encoded_generic =
        utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Encoding: "
                     "gzip\r\nContent-Length: 48\r\n\r\n");
    check(validate_head(CdnImageType::mp3, missing_type).error == CdnHttpError::invalid_content_type &&
              validate_head(CdnImageType::wav, empty_type).error == CdnHttpError::invalid_content_type &&
              validate_head(CdnImageType::wav, encoded_generic).error ==
                  CdnHttpError::invalid_content_encoding &&
              validate_head(CdnImageType::png, generic_head).error == CdnHttpError::invalid_content_type &&
              validate_head(CdnImageType::ogg_opus, generic_head).error ==
                  CdnHttpError::invalid_content_type &&
              validate_static_image_response(
                  CdnImageType::wav, {reinterpret_cast<const u8*>(generic_head.data), generic_head.bytes},
                  kCdnMaximumEncodedBodyBytes + 1, 1)
                      .error == CdnHttpError::body_too_large,
          "generic audio MIME retains missing type encoding and body bounds plus existing image and Opus "
          "policies");
    encoded_media_cache_erase(&cache, search_wave);
    check(!cache.entry_count && !cache.used_pages, "common audio sources release every bounded cache page");
}
} // namespace
void test_media_worker_publication(void (*check)(bool, const char*) noexcept) noexcept {
    deferred_cache_checks(check, false);
    deferred_cache_checks(check, true);
    retained_audio_checks(check);
    common_audio_checks(check);
    constexpr u32 page = 4096;
    constexpr AccountMediaWorkerPhase phases[]{
        AccountMediaWorkerPhase::empty,         AccountMediaWorkerPhase::idle,
        AccountMediaWorkerPhase::command_ready, AccountMediaWorkerPhase::working,
        AccountMediaWorkerPhase::leased,        AccountMediaWorkerPhase::stopped,
        AccountMediaWorkerPhase::failed};
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        auto* allocation = static_cast<u8*>(VirtualAlloc(nullptr, sizeof(AccountMediaWorker) + 2 * page,
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(allocation != nullptr, "media publication guard allocation");
        if (!allocation)
            return;
        const u32 offset = FIELD_OFFSET(AccountMediaWorker, lanes) +
                           index * sizeof(AccountMediaWorkerLane) +
                           FIELD_OFFSET(AccountMediaWorkerLane, completed_result) +
                           FIELD_OFFSET(AccountMediaRequestResult, state);
        const u32 shift = ((page - 8 - (offset & (page - 1))) & (page - 1)) & ~7u;
        auto* worker = reinterpret_cast<AccountMediaWorker*>(allocation + shift);
        worker->initialized = true;
        for (auto& lane : worker->lanes)
            lane.phase = static_cast<LONG>(AccountMediaWorkerPhase::idle);
        auto& lane = worker->lanes[index];
        lane.completed_result.state = AccountMediaRequestState::ready;
        const ULONG_PTR guarded =
            reinterpret_cast<ULONG_PTR>(&lane.completed_result.state) & ~ULONG_PTR{page - 1};
        const ULONG_PTR phase_page = reinterpret_cast<ULONG_PTR>(&lane.phase) & ~ULONG_PTR{page - 1};
        DWORD previous = 0;
        const bool isolated =
            guarded != phase_page &&
            VirtualProtect(reinterpret_cast<void*>(guarded), page, PAGE_NOACCESS, &previous) != FALSE;
        check(isolated, "media completion payload separated from publication phase");
        if (!isolated) {
            VirtualFree(allocation, 0, MEM_RELEASE);
            return;
        }
        // No payload access is allowed until the worker publishes output_ready.
        // A protected page makes the old read-before-phase ordering deterministic.
        for (auto phase : phases) {
            lane.phase = static_cast<LONG>(phase);
            for (u32 preferred = 0; preferred < 2; ++preferred) {
                worker->preferred_lane = preferred ? static_cast<LONG>(index) : -1;
                AccountMediaFrame frame{};
                frame.content_identity = 0xabcdef0123456789ull;
                const bool taken = account_media_worker_take_frame(worker, &frame);
                check(!taken && lane.phase == static_cast<LONG>(phase) && worker->preferred_lane == -1 &&
                          frame.content_identity == 0xabcdef0123456789ull,
                      "media unpublished payload remains unread and unclaimed");
            }
        }
        DWORD ignored = 0;
        const bool restored =
            VirtualProtect(reinterpret_cast<void*>(guarded), page, previous, &ignored) != FALSE;
        check(restored, "media publication guard restore");
        if (restored) {
            lane.phase = static_cast<LONG>(AccountMediaWorkerPhase::output_ready);
            lane.completed_result.state = AccountMediaRequestState::failed;
            AccountMediaFrame frame{};
            check(!account_media_worker_take_frame(worker, &frame) &&
                      lane.phase == static_cast<LONG>(AccountMediaWorkerPhase::output_ready),
                  "media failed completion stays available for failure consumer");
        }
        VirtualFree(allocation, 0, MEM_RELEASE);
    }
    AccountMediaWorker worker{};
    worker.initialized = true;
    worker.output_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(worker.output_event != nullptr, "media priority notification event");
    if (!worker.output_event)
        return;
    u8 pixel[4]{};
    // Out-of-range below and above, no preference, first, middle and last (wrapping) lanes.
    constexpr LONG preferences[]{-2, -1, 0, 3, 7, 8};
    bool ordered = true;
    for (LONG preferred : preferences)
        for (u32 mask = 0; mask < (1u << kAccountMediaDecodeLaneCount); ++mask) {
            ResetEvent(worker.output_event);
            worker.preferred_lane = preferred;
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
                auto& lane = worker.lanes[index];
                lane.phase =
                    static_cast<LONG>((mask & (1u << index)) ? AccountMediaWorkerPhase::output_ready
                                                             : AccountMediaWorkerPhase::idle);
                lane.completed_result.state = AccountMediaRequestState::ready;
                lane.completed_content_identity = index + 1;
                lane.media.frame_ready = true;
                lane.media.frame_kind = AccountMediaFrameKind::straight_rgba8;
                lane.media.frame_bytes = 4;
                lane.media.pipeline.scanline_storage = pixel;
                lane.media.frame_webp_image.width = lane.media.frame_webp_image.height = 1;
            }
            const u32 first = preferred >= 0 && preferred < static_cast<LONG>(kAccountMediaDecodeLaneCount)
                                  ? static_cast<u32>(preferred)
                                  : 0;
            u32 expected = kAccountMediaDecodeLaneCount;
            for (u32 step = 0; step < kAccountMediaDecodeLaneCount; ++step) {
                const u32 index = (first + step) % kAccountMediaDecodeLaneCount;
                if (mask & (1u << index)) {
                    expected = index;
                    break;
                }
            }
            AccountMediaFrame frame{};
            const bool taken = account_media_worker_take_frame(&worker, &frame);
            bool correct = taken == (mask != 0) && worker.preferred_lane == -1;
            if (taken)
                correct = correct && frame.content_identity == expected + 1 && frame.bytes.data == pixel;
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
                const auto expected_phase = index == expected        ? AccountMediaWorkerPhase::leased
                                            : (mask & (1u << index)) ? AccountMediaWorkerPhase::output_ready
                                                                     : AccountMediaWorkerPhase::idle;
                correct = correct && worker.lanes[index].phase == static_cast<LONG>(expected_phase);
            }
            const bool remaining = mask && (mask & (mask - 1));
            correct = correct && WaitForSingleObject(worker.output_event, 0) ==
                                     (remaining ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
            ordered = ordered && correct;
        }
    check(ordered, "media priority order and remaining frame notification");
    CloseHandle(worker.output_event);
}
} // namespace maicord

#include "maicord/media/ogg_opus.hpp"

namespace maicord {
namespace {
// Original synthetic container fixtures. TOC-only frames request Opus PLC.
// These tests inspect transport and timing, not decoder audio quality.
struct OggFixture {
    u8 bytes[4096]{};
    u32 count{}, pages[32]{}, page_count{};
};
void put32(u8* p, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i)
        p[i] = static_cast<u8>(value >> (8 * i));
}
void put64(u8* p, u64 value) noexcept {
    for (u32 i = 0; i < 8; ++i)
        p[i] = static_cast<u8>(value >> (8 * i));
}
u32 fixture_crc(const u8* p, u32 count) noexcept {
    u32 crc = 0;
    for (u32 i = 0; i < count; ++i) {
        crc ^= u32{i >= 22 && i < 26 ? u8{0} : p[i]} << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
    return crc;
}
template <typename Fixture> void fix_crc(Fixture& f, u32 index) noexcept {
    const u32 offset = f.pages[index];
    const u32 end = index + 1 < f.page_count ? f.pages[index + 1] : f.count;
    put32(f.bytes + offset + 22, fixture_crc(f.bytes + offset, end - offset));
}
template <typename Fixture>
bool append_page(Fixture& f, u8 flags, u64 granule, ConstSlice<u8> laces, ConstSlice<u8> body,
                 u32 serial = 23) noexcept {
    if (f.page_count >= 32 || laces.count > 255 ||
        27 + laces.count + body.count > sizeof(f.bytes) - f.count)
        return false;
    u32 data = 0;
    for (u32 i = 0; i < laces.count; ++i)
        data += laces[i];
    if (data != body.count)
        return false;
    const u32 start = f.count;
    auto* p = f.bytes + start;
    copy_bytes(p, "OggS", 4);
    p[4] = 0;
    p[5] = flags;
    put64(p + 6, granule);
    put32(p + 14, serial);
    put32(p + 18, f.page_count);
    p[26] = static_cast<u8>(laces.count);
    copy_bytes(p + 27, laces.data, laces.count);
    copy_bytes(p + 27 + laces.count, body.data, body.count);
    f.pages[f.page_count++] = start;
    f.count += 27 + laces.count + body.count;
    fix_crc(f, f.page_count - 1);
    return true;
}
template <typename Fixture>
bool headers(Fixture& f, u8 channels = 2, u16 skip = 312, i16 gain = -256) noexcept {
    u8 head[19]{};
    copy_bytes(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = channels;
    head[10] = static_cast<u8>(skip);
    head[11] = static_cast<u8>(skip >> 8);
    put32(head + 12, 44100);
    const u16 gain_bits = static_cast<u16>(gain);
    head[16] = static_cast<u8>(gain_bits);
    head[17] = static_cast<u8>(gain_bits >> 8);
    constexpr u8 head_lace[]{19}, tags_lace[]{16};
    u8 tags[16]{};
    copy_bytes(tags, "OpusTags", 8);
    return append_page(f, 2, 0, {head_lace, 1}, {head, 19}) &&
           append_page(f, 0, 0, {tags_lace, 1}, {tags, 16});
}
bool ordinary(OggFixture& f, u32 packets = 8, u64 origin = 0, u32 trim = 100) noexcept {
    if (!headers(f))
        return false;
    constexpr u8 lace[]{1}, packet[]{0xf8}; // CELT 20 ms, one empty frame.
    for (u32 i = 0; i < packets; ++i)
        if (!append_page(f, i + 1 == packets ? 4 : 0,
                         origin + u64{i + 1} * 960 - (i + 1 == packets ? trim : 0), {lace, 1}, {packet, 1}))
            return false;
    return true;
}
bool always_cancelled(void*) noexcept {
    return true;
}
struct CancelAfterPackets {
    u32 remaining{};
};
bool cancel_after_packets(void* opaque) noexcept {
    auto& state = *static_cast<CancelAfterPackets*>(opaque);
    if (!state.remaining)
        return true;
    --state.remaining;
    return false;
}
} // namespace

void test_ogg_opus(void (*check)(bool, const char*) noexcept) noexcept {
    static u8 scratch[kOggOpusMaximumPacketBytes];
    OggFixture fixture{};
    check(ordinary(fixture), "ogg_opus_original_fixture");
    OggOpusDemuxer d{};
    const auto initialize = [&](OggFixture& f, OggOpusDemuxer* out = nullptr) noexcept {
        return ogg_opus_demux_initialize(out ? out : &d, {f.bytes, f.count}, {scratch, sizeof(scratch)});
    };
    check(initialize(fixture) == OggOpusError::none && d.ready && d.channels == 2 && d.pre_skip == 312 &&
              d.output_gain_q8 == -256 && d.input_sample_rate == 44100 && d.total_frames == 7268 &&
              d.decoded_frames == 7680 && d.packet_count == 8 && !d.granule_origin,
          "ogg_opus_48k_metadata_preskip_gain_end_trim");
    if (!d.ready)
        return;
    u64 retained = 0;
    for (u32 i = 0; i < 8; ++i) {
        const auto p = ogg_opus_demux_next(&d, {});
        check(p.ok() && p.bytes.count == 1 && p.bytes.data >= fixture.bytes &&
                  p.bytes.data < fixture.bytes + fixture.count && p.packet_index == i &&
                  p.decoded_first_frame == u64{i} * 960 && p.duration_frames == 960 &&
                  p.trim_front_frames == (i ? 0u : 312u) && p.trim_back_frames == (i == 7 ? 100u : 0u),
              "ogg_opus_exact_packet_timing_borrowed_source");
        retained += p.duration_frames - p.trim_front_frames - p.trim_back_frames;
    }
    check(retained == d.total_frames && ogg_opus_demux_next(&d, {}).error == OggOpusError::end_of_stream,
          "ogg_opus_retained_duration_and_eof");
    OggOpusSeekPoint point{};
    check(ogg_opus_demux_seek(&d, 5000, &point, {scratch, sizeof(scratch)}) == OggOpusError::none &&
              point.packet_index == 1 && point.decoded_first_frame == 960 && point.target_frame == 5000 &&
              point.discard_frames == 4352 && d.cursor.packet_index == 1,
          "ogg_opus_seek_full_80ms_preroll_and_exact_discard");
    const auto cursor = d.cursor;
    point.target_frame = 99;
    check(ogg_opus_demux_seek(&d, 4000, &point, {}, always_cancelled) == OggOpusError::cancelled &&
              bytes_equal(&cursor, &d.cursor, sizeof(cursor)) && point.target_frame == 99,
          "ogg_opus_seek_cancel_is_transactional");
    check(ogg_opus_demux_seek(&d, d.total_frames + 1, &point, {}) == OggOpusError::invalid_input &&
              bytes_equal(&cursor, &d.cursor, sizeof(cursor)) && point.target_frame == 99,
          "ogg_opus_seek_range_is_transactional");
    check(ogg_opus_demux_seek(&d, 1, &point, {}) == OggOpusError::none && !point.packet_index &&
              point.discard_frames == 313,
          "ogg_opus_seek_near_start_uses_preskip");
    check(ogg_opus_demux_seek(&d, d.total_frames, &point, {}) == OggOpusError::none,
          "ogg_opus_seek_endpoint_is_admitted");
    ogg_opus_demux_reset(&d);
    check(!d.cursor.packet_index && !d.cursor.decoded_frame, "ogg_opus_reset_original_cursor");

    OggFixture cropped{};
    check(ordinary(cropped, 8, 10000), "ogg_opus_cropped_fixture");
    check(initialize(cropped) == OggOpusError::none && d.granule_origin == 10000 && d.total_frames == 7268,
          "ogg_opus_nonzero_initial_granule_does_not_add_silence");
    OggFixture single{};
    check(ordinary(single, 1, 0, 100), "ogg_opus_single_eos_fixture");
    check(initialize(single) == OggOpusError::none && d.total_frames == 548,
          "ogg_opus_first_audio_page_eos_trim_exception");
    OggFixture continued{};
    check(headers(continued, 1, 0, 512), "ogg_opus_continued_headers");
    u8 long_packet[300]{};
    long_packet[0] = 0xf8;
    constexpr u8 first_lace[]{255}, last_lace[]{45};
    check(append_page(continued, 0, ~u64{0}, {first_lace, 1}, {long_packet, 255}) &&
              append_page(continued, 5, 960, {last_lace, 1}, {long_packet + 255, 45}),
          "ogg_opus_continued_fixture");
    check(initialize(continued) == OggOpusError::none && d.channels == 1 && d.output_gain_q8 == 512,
          "ogg_opus_continued_crc_lacing_admission");
    const auto before_capacity = d.cursor;
    check(ogg_opus_demux_next(&d, {}).error == OggOpusError::capacity &&
              bytes_equal(&before_capacity, &d.cursor, sizeof(before_capacity)),
          "ogg_opus_continued_missing_scratch_preserves_cursor");
    const auto assembled = ogg_opus_demux_next(&d, {scratch, sizeof(scratch)});
    check(assembled.ok() && assembled.bytes.data == scratch && assembled.bytes.count == 300 &&
              bytes_equal(assembled.bytes.data, long_packet, 300) && assembled.duration_frames == 960,
          "ogg_opus_continued_exact_assembly_borrowed_scratch");

    OggFixture broken = fixture;
    const auto saved = d;
    broken.bytes[broken.count - 1] ^= 1;
    check(initialize(broken) == OggOpusError::checksum && bytes_equal(&saved, &d, sizeof(d)),
          "ogg_opus_checksum_failure_preserves_metadata");
    broken = fixture;
    --broken.count;
    check(initialize(broken) == OggOpusError::truncated, "ogg_opus_truncated_page");
    broken = fixture;
    broken.bytes[broken.pages[2] + 5] = 1;
    fix_crc(broken, 2);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_orphan_continuation_rejected");
    broken = fixture;
    put32(broken.bytes + broken.pages[3] + 18, 99);
    fix_crc(broken, 3);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_sequence_gap_rejected");
    broken = fixture;
    put32(broken.bytes + broken.pages[3] + 14, 24);
    fix_crc(broken, 3);
    check(initialize(broken) == OggOpusError::unsupported, "ogg_opus_multiplexing_rejected");
    broken = fixture;
    put64(broken.bytes + broken.pages[3] + 6, 1919);
    fix_crc(broken, 3);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_granule_discontinuity_rejected");
    broken = fixture;
    put64(broken.bytes + broken.pages[2] + 6, 1);
    fix_crc(broken, 2);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_initial_granule_underflow_rejected");
    broken = single;
    put64(broken.bytes + broken.pages[2] + 6, 311);
    fix_crc(broken, 2);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_end_before_preskip_rejected");
    broken = fixture;
    broken.bytes[28 + 18] = 1;
    fix_crc(broken, 0);
    check(initialize(broken) == OggOpusError::unsupported, "ogg_opus_mapping_family_rejected");
    broken = fixture;
    put32(broken.bytes + broken.pages[1] + 28 + 8, 0xffffffffu);
    fix_crc(broken, 1);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_tag_length_overflow_rejected");
    broken = fixture;
    broken.bytes[broken.pages[9] + 5] = 0;
    fix_crc(broken, 9);
    check(initialize(broken) == OggOpusError::malformed, "ogg_opus_end_trim_without_eos_rejected");
    // Removing EOS also removes permission to shorten the final granule. Use
    // the complete decoded count to isolate a missing terminal marker.
    put64(broken.bytes + broken.pages[9] + 6, 8u * 960u);
    fix_crc(broken, 9);
    check(initialize(broken) == OggOpusError::truncated, "ogg_opus_missing_eos_rejected");
    broken = fixture;
    constexpr u8 one_lace[]{1}, one_packet[]{0xf8};
    check(append_page(broken, 2, 0, {one_lace, 1}, {one_packet, 1}), "ogg_opus_chained_fixture");
    check(initialize(broken) == OggOpusError::unsupported, "ogg_opus_chaining_rejected");
    check(ogg_opus_demux_initialize(&d, {fixture.bytes, fixture.count}, {}, always_cancelled) ==
              OggOpusError::cancelled,
          "ogg_opus_initialize_cancelled");
    check(ogg_opus_demux_initialize(&d, {fixture.bytes, fixture.count}, {fixture.bytes, 1}) ==
              OggOpusError::invalid_input,
          "ogg_opus_source_scratch_alias_rejected");

    OggFixture tag_continuation{};
    u8 head[19]{};
    copy_bytes(head, "OpusHead", 8);
    head[8] = head[9] = 1;
    u8 large_tag[300]{};
    copy_bytes(large_tag, "OpusTags", 8);
    put32(large_tag + 8, 284);
    constexpr u8 head_lace[]{19};
    check(append_page(tag_continuation, 2, 0, {head_lace, 1}, {head, 19}) &&
              append_page(tag_continuation, 0, ~u64{0}, {first_lace, 1}, {large_tag, 255}) &&
              append_page(tag_continuation, 1, 0, {last_lace, 1}, {large_tag + 255, 45}) &&
              append_page(tag_continuation, 4, 960, {one_lace, 1}, {one_packet, 1}),
          "ogg_opus_continued_tags_fixture");
    check(initialize(tag_continuation) == OggOpusError::none && d.packet_count == 1 &&
              d.total_frames == 960,
          "ogg_opus_continued_tags_exact_length_validation");

    OggFixture grouped{};
    constexpr u8 grouped_laces[]{1, 1}, grouped_packets[]{0xf8, 0xf8};
    check(headers(grouped, 2, 1500, 0) &&
              append_page(grouped, 0, 1920, {grouped_laces, 2}, {grouped_packets, 2}) &&
              append_page(grouped, 4, 2750, {one_lace, 1}, {one_packet, 1}) &&
              initialize(grouped) == OggOpusError::none && d.total_frames == 1250,
          "ogg_opus_multiple_packets_on_page_and_preskip_across_packets");
    const auto grouped_first = ogg_opus_demux_next(&d, {});
    const auto grouped_second = ogg_opus_demux_next(&d, {});
    const auto grouped_last = ogg_opus_demux_next(&d, {});
    check(grouped_first.ok() && grouped_second.ok() && grouped_last.ok() &&
              grouped_first.trim_front_frames == 960 && grouped_second.trim_front_frames == 540 &&
              grouped_second.decoded_first_frame == 960 && grouped_last.trim_back_frames == 130,
          "ogg_opus_grouped_cursor_exact_front_back_windows");

    OggFixture dense{};
    u8 dense_laces[255], dense_packets[510];
    for (auto& lace : dense_laces)
        lace = 1;
    for (u32 i = 0; i < 510; ++i)
        dense_packets[i] = i & 1 ? 0xf0 : 0xf8; // Alternating 10/20 ms PLC frames.
    constexpr u64 dense_first_frames = 183840, dense_frames = 367200, dense_trim = 17;
    const bool dense_ready =
        headers(dense, 1, 0, 0) &&
        append_page(dense, 0, dense_first_frames, {dense_laces, 255}, {dense_packets, 255}) &&
        append_page(dense, 4, dense_frames - dense_trim, {dense_laces, 255}, {dense_packets + 255, 255}) &&
        initialize(dense) == OggOpusError::none;
    check(dense_ready && d.packet_count == 510 && d.decoded_frames == dense_frames &&
              d.total_frames == dense_frames - dense_trim,
          "ogg_opus_dense_two_full_lacing_pages_fixture");
    if (dense_ready) {
        u64 first_frame = 0, dense_retained = 0;
        bool exact_packets = true, exact_geometry = true;
        for (u32 i = 0; i < 510; ++i) {
            const auto packet = ogg_opus_demux_next(&d, {});
            const u32 page_index = i < 255 ? 2 : 3;
            const u32 body = dense.pages[page_index] + 27 + 255;
            const u32 end = page_index == 2 ? dense.pages[3] : dense.count;
            const u32 duration = i & 1 ? 480 : 960;
            exact_packets &= packet.ok() && packet.packet_index == i && packet.bytes.count == 1 &&
                             packet.bytes.data == dense.bytes + body + i % 255 &&
                             packet.bytes[0] == dense_packets[i] && packet.duration_frames == duration &&
                             packet.decoded_first_frame == first_frame && !packet.trim_front_frames &&
                             packet.trim_back_frames == (i == 509 ? dense_trim : 0);
            // All 255 packets retain one page's geometry; crossing the boundary
            // installs the next page, without a retained source-wide index.
            exact_geometry &= d.cursor.page_offset == dense.pages[page_index] &&
                              d.cursor.page_body == body && d.cursor.page_end == end &&
                              d.cursor.segment_index == i % 255 + 1 &&
                              d.cursor.data_offset == body + i % 255 + 1;
            dense_retained += duration - (i == 509 ? dense_trim : 0);
            first_frame += duration;
        }
        check(exact_packets && dense_retained == d.total_frames &&
                  ogg_opus_demux_next(&d, {}).error == OggOpusError::end_of_stream,
              "ogg_opus_dense_exact_packets_durations_end_trim_and_eof");
        check(exact_geometry, "ogg_opus_dense_current_page_geometry_reused_and_replaced");
        check(ogg_opus_demux_seek(&d, 100000, &point, {}) == OggOpusError::none &&
                  point.packet_index == 133 && point.decoded_first_frame == 96000 &&
                  point.discard_frames == 4000 && d.cursor.page_offset == dense.pages[2] &&
                  d.cursor.page_body == dense.pages[2] + 282 && d.cursor.page_end == dense.pages[3],
              "ogg_opus_dense_seek_first_page_exact_preroll_cached_geometry");
        auto invalid_cache = d;
        ++invalid_cache.cursor.page_body;
        const auto invalid_cache_before = invalid_cache.cursor;
        check(ogg_opus_demux_next(&invalid_cache, {}).error == OggOpusError::malformed &&
                  bytes_equal(&invalid_cache.cursor, &invalid_cache_before, sizeof(invalid_cache_before)),
              "ogg_opus_cached_header_geometry_mismatch_is_transactional");
        invalid_cache = d;
        invalid_cache.cursor.page_end = dense.count + 1;
        const auto invalid_end_before = invalid_cache.cursor;
        check(ogg_opus_demux_next(&invalid_cache, {}).error == OggOpusError::malformed &&
                  bytes_equal(&invalid_cache.cursor, &invalid_end_before, sizeof(invalid_end_before)),
              "ogg_opus_cached_page_end_outside_source_is_transactional");
        check(ogg_opus_demux_seek(&d, 260000, &point, {}) == OggOpusError::none &&
                  point.packet_index == 355 && point.decoded_first_frame == 255840 &&
                  point.discard_frames == 4160 && d.cursor.page_offset == dense.pages[3] &&
                  d.cursor.page_body == dense.pages[3] + 282 && d.cursor.page_end == dense.count,
              "ogg_opus_dense_seek_second_page_exact_preroll_cached_geometry");
        const auto dense_cursor = d.cursor;
        const auto dense_point = point;
        CancelAfterPackets cancellation{300};
        check(ogg_opus_demux_seek(&d, 350000, &point, {}, cancel_after_packets, &cancellation) ==
                      OggOpusError::cancelled &&
                  !cancellation.remaining && bytes_equal(&d.cursor, &dense_cursor, sizeof(dense_cursor)) &&
                  bytes_equal(&point, &dense_point, sizeof(dense_point)),
              "ogg_opus_dense_seek_cancel_after_page_transition_preserves_cached_cursor_and_output");
        const auto seek_packet = ogg_opus_demux_next(&d, {});
        check(seek_packet.ok() && seek_packet.packet_index == 355 && seek_packet.duration_frames == 480 &&
                  seek_packet.bytes.data == dense.bytes + dense.pages[3] + 282 + 100,
              "ogg_opus_dense_cancelled_seek_keeps_exact_next_packet");
        ogg_opus_demux_reset(&d);
        check(!d.cursor.page_body && !d.cursor.page_end && !d.cursor.data_offset &&
                  !d.cursor.segment_index && !d.cursor.packet_index && !d.cursor.decoded_frame &&
                  d.cursor.page_offset == dense.pages[2],
              "ogg_opus_dense_reset_clears_only_current_page_cache");
        const auto reset_packet = ogg_opus_demux_next(&d, {});
        check(reset_packet.ok() && !reset_packet.packet_index && reset_packet.duration_frames == 960 &&
                  d.cursor.page_end == dense.pages[3],
              "ogg_opus_dense_reset_repopulates_geometry_from_first_audio_page");
    }

    OggFixture toc{};
    check(headers(toc, 1, 0, 0), "ogg_opus_toc_headers");
    constexpr u8 two_laces[]{2}, six_frames[]{0xfb, 6};
    check(append_page(toc, 4, 5760, {two_laces, 1}, {six_frames, 2}) &&
              initialize(toc) == OggOpusError::none && d.total_frames == 5760,
          "ogg_opus_120ms_packet_exact_duration");
    auto invalid_toc = toc;
    invalid_toc.bytes[invalid_toc.pages[2] + 29] = 0;
    fix_crc(invalid_toc, 2);
    check(initialize(invalid_toc) == OggOpusError::malformed, "ogg_opus_zero_frame_count_rejected");
    invalid_toc = toc;
    invalid_toc.bytes[invalid_toc.pages[2] + 29] = 7;
    fix_crc(invalid_toc, 2);
    check(initialize(invalid_toc) == OggOpusError::malformed, "ogg_opus_over_120ms_rejected");
    invalid_toc = toc;
    invalid_toc.bytes[invalid_toc.pages[2] + 28] = 0xf9;
    fix_crc(invalid_toc, 2);
    check(initialize(invalid_toc) == OggOpusError::malformed, "ogg_opus_odd_cbr_payload_rejected");

    struct LargeFixture {
        u8 bytes[66000]{};
        u32 count{}, pages[32]{}, page_count{};
    };
    static LargeFixture large{};
    static u8 bounded_packet[kOggOpusMaximumPacketBytes + 1];
    bounded_packet[0] = 0xfb;
    bounded_packet[1] = 65;
    for (u32 i = 2; i < 242; ++i)
        bounded_packet[i] = 255;
    bounded_packet[242] = 237; // 240*254+237 padding bytes, one empty 20 ms frame.
    u8 laces[240];
    for (auto& lace : laces)
        lace = 255;
    constexpr u8 tail[]{240};
    check(headers(large, 1, 0, 0) &&
              append_page(large, 0, ~u64{0}, {laces, 240}, {bounded_packet, 61200}) &&
              append_page(large, 5, 960, {tail, 1}, {bounded_packet + 61200, 240}),
          "ogg_opus_maximum_packet_fixture");
    check(ogg_opus_demux_initialize(&d, {large.bytes, large.count}, {scratch, sizeof(scratch)}) ==
              OggOpusError::none,
          "ogg_opus_61440_packet_supported");
    const auto maximum_packet = ogg_opus_demux_next(&d, {scratch, sizeof(scratch)});
    check(maximum_packet.ok() && maximum_packet.bytes.count == kOggOpusMaximumPacketBytes &&
              maximum_packet.duration_frames == 960 &&
              bytes_equal(maximum_packet.bytes.data, bounded_packet, kOggOpusMaximumPacketBytes),
          "ogg_opus_maximum_continued_packet_exact_bytes");
    large = {};
    bounded_packet[242] = 238;
    constexpr u8 oversized_tail[]{241};
    check(headers(large, 1, 0, 0) &&
              append_page(large, 0, ~u64{0}, {laces, 240}, {bounded_packet, 61200}) &&
              append_page(large, 5, 960, {oversized_tail, 1}, {bounded_packet + 61200, 241}),
          "ogg_opus_oversized_packet_fixture");
    check(ogg_opus_demux_initialize(&d, {large.bytes, large.count}, {scratch, sizeof(scratch)}) ==
              OggOpusError::capacity,
          "ogg_opus_packet_over_bound_rejected");
}
} // namespace maicord

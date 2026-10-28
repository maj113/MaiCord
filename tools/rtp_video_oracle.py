"""Generate RTP video receive fixtures (stdlib only).

Builds tiny synthetic H.264 / H.265 / AV1 elementary streams, packetizes them
independently of MaiCord (RFC 6184 packetization-mode 1, RFC 7798 without
DONL, the AOM AV1 RTP payload as WebRTC writes it), and writes packet sets with loss/reorder/duplicate/RTX permutations,
the byte-exact Annex B (4-byte start code) access units, or AV1 temporal
units in Discord's depacketized form, the receiver must emit, and RTCP NACK/PLI/FIR/RR/SR serializations.

    python tools/rtp_video_oracle.py tests/data/rtp_video_vectors.hpp
"""
import argparse
import random
import struct
from pathlib import Path

MTU = 1100  # Maximum RTP payload bytes used by the reference packetizer.
SSRC = 0x1234ABCD
RTX_SSRC = SSRC + 1
LOCAL_SSRC = 0x00C0FFEE
CODECS = {'h264': 101, 'h265': 103, 'av1': 105}


def nal_body(rng, size, first_bit):
    # Bytes 1..255 avoid accidental start codes; the first slice byte's MSB
    # encodes first_mb_in_slice == 0 / first_slice_segment_in_pic_flag.
    body = bytearray(rng.randrange(1, 256) for _ in range(size))
    if first_bit is not None:
        body[0] = (body[0] | 0x80) if first_bit else ((body[0] & 0x7F) or 0x11)
    return bytes(body)


def h264_nal(rng, nal_type, size, first_bit=None, nri=3):
    return bytes([(nri << 5) | nal_type]) + nal_body(rng, size, first_bit)


def h265_nal(rng, nal_type, size, first_bit=None):
    return bytes([nal_type << 1, 1]) + nal_body(rng, size, first_bit)


def h264_stream(rng):
    sps, pps = h264_nal(rng, 7, 12), h264_nal(rng, 8, 4)
    return [
        [sps, pps, h264_nal(rng, 5, 3300, True)],                       # IDR: STAP-A + FU-A
        [h264_nal(rng, 1, 700, True)],                                  # single NAL
        [h264_nal(rng, 1, 1500, True), h264_nal(rng, 1, 900, False)],   # two slices
        [h264_nal(rng, 6, 20), h264_nal(rng, 1, 2400, True, nri=2)],    # SEI + FU-A
        [h264_nal(rng, 1, 90, True)],
        [h264_nal(rng, 1, 1800, True)],
    ]


def h265_stream(rng):
    vps, sps, pps = h265_nal(rng, 32, 20), h265_nal(rng, 33, 30), h265_nal(rng, 34, 6)
    return [
        [vps, sps, pps, h265_nal(rng, 19, 3000, True)],                 # IDR_W_RADL
        [h265_nal(rng, 1, 600, True)],
        [h265_nal(rng, 1, 1300, True), h265_nal(rng, 1, 400, False)],
        [h265_nal(rng, 39, 15), h265_nal(rng, 1, 2500, True)],          # prefix SEI + FU
        [h265_nal(rng, 1, 70, True)],
        [h265_nal(rng, 0, 1700, True)],
    ]


def packetize_h264(nals):
    payloads, pending = [], []

    def flush():
        if len(pending) == 1:
            payloads.append(pending[0])
        elif pending:
            nri = max(n[0] & 0x60 for n in pending)
            payloads.append(bytes([nri | 24]) + b''.join(struct.pack('>H', len(n)) + n for n in pending))
        pending.clear()

    for nal in nals:
        if len(nal) + 3 <= MTU and sum(len(n) + 2 for n in pending) + len(nal) + 3 <= MTU:
            pending.append(nal)
            continue
        flush()
        if len(nal) <= MTU:
            pending.append(nal)
            continue
        indicator = (nal[0] & 0xE0) | 28
        data, offset = nal[1:], 0
        while offset < len(data):
            chunk = data[offset:offset + MTU - 2]
            header = nal[0] & 0x1F
            if offset == 0:
                header |= 0x80
            if offset + len(chunk) == len(data):
                header |= 0x40
            payloads.append(bytes([indicator, header]) + chunk)
            offset += len(chunk)
    flush()
    return payloads


def packetize_h265(nals):
    payloads, pending = [], []

    def flush():
        if len(pending) == 1:
            payloads.append(pending[0])
        elif pending:
            # AP header: F=0, Type=48, lowest LayerId/TID of the aggregated units.
            payloads.append(bytes([48 << 1, 1]) + b''.join(struct.pack('>H', len(n)) + n for n in pending))
        pending.clear()

    for nal in nals:
        if sum(len(n) + 2 for n in pending) + len(nal) + 4 <= MTU:
            pending.append(nal)
            continue
        flush()
        if len(nal) <= MTU:
            pending.append(nal)
            continue
        nal_type = (nal[0] >> 1) & 0x3F
        header = bytes([(nal[0] & 0x81) | (49 << 1), nal[1]])
        data, offset = nal[2:], 0
        while offset < len(data):
            chunk = data[offset:offset + MTU - 3]
            fu = nal_type
            if offset == 0:
                fu |= 0x80
            if offset + len(chunk) == len(data):
                fu |= 0x40
            payloads.append(header + bytes([fu]) + chunk)
            offset += len(chunk)
    flush()
    return payloads


def av1_obu(rng, obu_type, size, extension=None):
    header = bytes([obu_type << 3 | (4 if extension is not None else 0)])
    if extension is not None:
        header += bytes([extension])
    return header, bytes(rng.randrange(256) for _ in range(size))


def av1_stream(rng):
    # Temporal delimiters are dropped by the packetizer; payloads are opaque here.
    td = (bytes([2 << 3]), b'')
    return [
        [td, av1_obu(rng, 1, 12), av1_obu(rng, 6, 3300)],                     # sequence header + key frame
        [td, av1_obu(rng, 6, 700)],
        [td, av1_obu(rng, 3, 40), av1_obu(rng, 4, 1500), av1_obu(rng, 4, 900)],  # frame header + tile groups
        [td, av1_obu(rng, 6, 30), av1_obu(rng, 6, 2400)],                     # hidden frame + shown frame
        [td, av1_obu(rng, 6, 90, extension=0x20)],                            # temporal_id 1
        [td, av1_obu(rng, 6, 1800)],
    ]


def leb128(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def av1_kept(obus):
    return [(h, p) for h, p in obus if (h[0] >> 3) & 15 not in (2, 8, 15)]


def packetize_av1(obus):
    # WebRTC RtpPacketizerAv1: size fields stripped, W counts up to three
    # elements (the last then has no length), Z/Y mark fragments, N a sequence start.
    kept = av1_kept(obus)
    packets, current, used, continues = [], [], 1, False

    def flush(more):
        nonlocal current, used, continues
        packets.append((continues, more, current))
        current, used, continues = [], 1, more

    for header, payload in kept:
        data, offset = bytes([header[0] & ~2]) + header[1:] + payload, 0
        while offset < len(data):
            room = MTU - used - 2
            if room < 8:
                flush(offset > 0)
                continue
            chunk = data[offset:offset + room]
            current.append(chunk)
            used += len(chunk) + len(leb128(len(chunk)))
            offset += len(chunk)
    flush(False)
    payloads = []
    for index, (z, y, elements) in enumerate(packets):
        w = len(elements) if len(elements) <= 3 else 0
        n = index == 0 and (kept[0][0][0] >> 3) & 15 == 1
        body = b''.join(e if w and i + 1 == len(elements) else leb128(len(e)) + e for i, e in enumerate(elements))
        payloads.append(bytes([z << 7 | y << 6 | w << 4 | n << 3]) + body)
    return payloads


def av1_frame(obus):
    kept, out = av1_kept(obus), bytearray()
    for index, (header, payload) in enumerate(kept):
        if index + 1 < len(kept):
            out += bytes([header[0] | 2]) + header[1:] + leb128(len(payload)) + payload
        else:
            out += bytes([header[0] & ~2]) + header[1:] + payload
    return bytes(out)


def rtp(pt, seq, ts, ssrc, marker, payload):
    return struct.pack('>BBHII', 0x80, (0x80 if marker else 0) | pt, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + payload


def annex_b(nals):
    return b''.join(b'\0\0\0\1' + n for n in nals)


class Stream:
    def __init__(self, codec, units, first_seq, first_ts):
        self.codec = codec
        packetize = {'h264': packetize_h264, 'h265': packetize_h265, 'av1': packetize_av1}[codec]
        self.packets, self.frame_of, self.frames = [], [], []
        seq, ts = first_seq, first_ts
        for index, nals in enumerate(units):
            payloads = packetize(nals)
            for i, payload in enumerate(payloads):
                self.packets.append((seq & 0xFFFF, ts, i + 1 == len(payloads), payload))
                self.frame_of.append(index)
                seq += 1
            self.frames.append(av1_frame(nals) if codec == 'av1' else annex_b(nals))
            ts += 3000
        self.pt = CODECS[codec]

    def media(self, index):
        seq, ts, marker, payload = self.packets[index]
        return rtp(self.pt, seq, ts, SSRC, marker, payload)

    def first_packet(self, frame):
        return self.frame_of.index(frame)


# Event kinds understood by tests/video_rtp_receive.cpp.
MEDIA, RTX, ADVANCE = 0, 1, 2


def scenarios(stream, rng):
    count = len(stream.packets)
    frames = list(range(len(stream.frames)))
    out = []
    # (name, events, delivered frames, keyframe-request expected)
    out.append(('in order', [(MEDIA, i) for i in range(count)], frames, False))
    order = list(range(count))
    for start in range(0, count, 6):
        block = order[start:start + 6]
        rng.shuffle(block)
        order[start:start + 6] = block
    out.append(('reordered', [(MEDIA, i) for i in order], frames, False))
    events = []
    for i in range(count):
        events.append((MEDIA, i))
        if i % 4 == 1:
            events.append((MEDIA, i))
    events.append((MEDIA, 2))  # Late duplicate after its unit was emitted.
    out.append(('duplicated', events, frames, False))
    lost = {3, stream.first_packet(3) + 1}
    events = []
    for i in range(count):
        if i not in lost:
            events.append((MEDIA, i))
        if i - 2 in lost:
            events.append((RTX, i - 2))
    out.append(('rtx recovery', events, frames, False))
    # Unrecovered loss inside unit 2: unit 2 is dropped after the give-up time;
    # unit 3 begins with a detectable access-unit start and is delivered.
    drop = stream.first_packet(2) + 1
    events = [(MEDIA, i) for i in range(count) if i != drop]
    split = events.index((MEDIA, stream.first_packet(3)))
    events.insert(split + 1, (ADVANCE, 400))
    out.append(('unrecovered loss', events, [0, 1, 3, 4, 5], True))
    # The first packet of unit 4 is lost: its remainder is discarded.
    drop = stream.first_packet(4)
    events = [(MEDIA, i) for i in range(count) if i != drop] + [(ADVANCE, 400)]
    out.append(('lost unit start', events, [0, 1, 2, 3, 5], True))
    # Joining mid-stream: the first packets are a unit tail.
    skip = stream.first_packet(1) + 1 if len(stream.packets) > stream.first_packet(1) + 1 and \
        stream.frame_of[stream.first_packet(1) + 1] == 1 else stream.first_packet(2) + 1
    events = [(MEDIA, i) for i in range(skip, count)]
    delivered = [f for f in frames if stream.first_packet(f) >= skip]
    out.append(('mid-stream join', events, delivered, True))
    return out


def rtcp_vectors():
    rr = struct.pack('>BBHI', 0x82, 201, 1 + 12, LOCAL_SSRC)
    rr += struct.pack('>IIIIII', SSRC, (25 << 24) | 7, 0x0001FFFF, 123, 0x89ABCDEF, 0x00028000)
    rr += struct.pack('>IIIIII', RTX_SSRC + 9, (0 << 24) | (0xFFFFFF & -3), 42, 0, 0, 0)
    pli = struct.pack('>BBHII', 0x81, 206, 2, LOCAL_SSRC, SSRC)
    fir = struct.pack('>BBHIIII', 0x84, 206, 4, LOCAL_SSRC, 0, SSRC, 9 << 24)
    sequences = [65534, 65535, 0, 3, 17, 18, 40, 41, 57, 58]
    items, i = [], 0
    while i < len(sequences):
        pid, blp = sequences[i], 0
        i += 1
        while i < len(sequences) and 0 < (sequences[i] - pid) & 0xFFFF <= 16:
            blp |= 1 << (((sequences[i] - pid) & 0xFFFF) - 1)
            i += 1
        items.append((pid, blp))
    nack = struct.pack('>BBHII', 0x81, 205, 2 + len(items), LOCAL_SSRC, SSRC)
    nack += b''.join(struct.pack('>HH', p, b) for p, b in items)
    sr = struct.pack('>BBHI', 0x80, 200, 6, SSRC) + struct.pack('>IIIII', 0xE1234567, 0x89ABCDEF, 90000, 10, 9000)
    compound = sr + struct.pack('>BBHI', 0x81, 202, 1, SSRC)  # SR + empty-ish SDES header
    return rr, pli, fir, sequences, nack, items, compound


def cstr(data):
    return '"' + data.hex() + '"'


def emit_stream(lines, name, stream, rng):
    lines.append(f'constexpr const char* {name}_packets[]{{')
    lines += [f'    {cstr(stream.media(i))},' for i in range(len(stream.packets))]
    lines.append('};')
    lines.append(f'constexpr const char* {name}_frames[]{{')
    lines += [f'    {cstr(frame)},' for frame in stream.frames]
    lines.append('};')
    cases = scenarios(stream, rng)
    rtx_seq = 7000
    for index, (title, events, delivered, keyframe) in enumerate(cases):
        lines.append(f'constexpr RtpVideoEvent {name}_events_{index}[]{{')
        for kind, value in events:
            if kind == RTX:
                lines.append(f'    {{{kind}, {value}, {rtx_seq}}},')
                rtx_seq += 1
            else:
                lines.append(f'    {{{kind}, {value}, 0}},')
        lines.append('};')
        lines.append(f'constexpr u8 {name}_delivered_{index}[]{{{", ".join(map(str, delivered))}}};')
    lines.append(f'constexpr RtpVideoScenario {name}_scenarios[]{{')
    for index, (title, events, delivered, keyframe) in enumerate(cases):
        lines.append(f'    {{"{name} {title}", {name}_events_{index}, {len(events)}, '
                     f'{name}_delivered_{index}, {len(delivered)}, {"true" if keyframe else "false"}}},')
    lines.append('};')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    rng = random.Random(0x5EED)
    # The H.264 stream starts near the 16-bit wrap so unwrapping is exercised.
    h264 = Stream('h264', h264_stream(rng), 65500, 0xFFFFF000)
    h265 = Stream('h265', h265_stream(rng), 1000, 90000)
    lines = ['// Generated by tools/rtp_video_oracle.py (stdlib reference packetizer).',
             f'constexpr u32 kOracleSsrc = 0x{SSRC:08X}u, kOracleRtxSsrc = 0x{RTX_SSRC:08X}u,',
             f'              kOracleLocalSsrc = 0x{LOCAL_SSRC:08X}u;',
             'struct RtpVideoEvent { u8 kind; u16 value, rtx_sequence; };',
             'struct RtpVideoScenario { const char* name; const RtpVideoEvent* events; u32 event_count;',
             '    const u8* delivered; u32 delivered_count; bool keyframe_request; };']
    emit_stream(lines, 'h264', h264, rng)
    emit_stream(lines, 'h265', h265, rng)
    # A separate generator keeps the H.264/H.265 vectors unchanged.
    av1_rng = random.Random(0xA71)
    av1 = Stream('av1', av1_stream(av1_rng), 30000, 0x7FFFF000)
    emit_stream(lines, 'av1', av1, av1_rng)
    rr, pli, fir, sequences, nack, items, compound = rtcp_vectors()
    lines.append(f'constexpr const char* rtcp_rr = {cstr(rr)};')
    lines.append(f'constexpr const char* rtcp_pli = {cstr(pli)};')
    lines.append(f'constexpr const char* rtcp_fir = {cstr(fir)};')
    lines.append(f'constexpr u16 rtcp_nack_sequences[]{{{", ".join(map(str, sequences))}}};')
    lines.append(f'constexpr u32 rtcp_nack_items = {len(items)};')
    lines.append(f'constexpr const char* rtcp_nack = {cstr(nack)};')
    lines.append(f'constexpr const char* rtcp_sr_compound = {cstr(compound)};')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print(f'Generated {len(h264.packets)} H.264, {len(h265.packets)} H.265 and {len(av1.packets)} AV1 packets: '
          f'{args.output}')


if __name__ == '__main__':
    main()

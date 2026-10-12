"""Write the deterministic HTTP/Gateway/PNG inflate corpus read by MaiCordHttpCompressionBenchmark.

Entry: u32 raw_size, u32 encoded_size, u32 kind, raw bytes, encoded bytes.
kind 0 = gzip, 1 = zlib, 2 = zlib stream: encoded bytes are u32 count, count
pairs of (raw, encoded) sizes, then each Z_SYNC_FLUSH segment in order.
"""
import json
import random
import struct
import sys
import zlib

rng = random.Random(151)
words = ('the a message reply channel guild role voice emoji sticker thread pinned '
         'hello world lol ok thanks screenshot link attachment server update').split()


def snowflake():
    return str(rng.randrange(1 << 50, 1 << 62))


def user():
    return {'id': snowflake(), 'username': ''.join(rng.choice('abcdefghijklmnopqrstuvwxyz_') for _ in range(rng.randrange(4, 14))),
            'global_name': None if rng.random() < .3 else ' '.join(rng.choice(words) for _ in range(2)),
            'avatar': '%032x' % rng.getrandbits(128), 'discriminator': '0', 'public_flags': rng.choice([0, 64, 128, 256]),
            'avatar_decoration_data': None, 'clan': None}


def message(channel):
    return {'type': 0, 'tts': False, 'timestamp': '2026-10-0%dT%02d:%02d:%02d.%06d+00:00' % (
        rng.randrange(1, 7), rng.randrange(24), rng.randrange(60), rng.randrange(60), rng.randrange(10 ** 6)),
        'pinned': False, 'mentions': [], 'mention_roles': [], 'mention_everyone': False, 'id': snowflake(),
        'flags': 0, 'embeds': [], 'edited_timestamp': None,
        'content': ' '.join(rng.choice(words) for _ in range(rng.randrange(1, 40))),
        'components': [], 'channel_id': channel, 'author': user(), 'attachments': []}


def ready():
    guilds = []
    for _ in range(60):
        guild = snowflake()
        guilds.append({'id': guild, 'name': ' '.join(rng.choice(words) for _ in range(3)), 'icon': '%032x' % rng.getrandbits(128),
                       'roles': [{'id': snowflake(), 'name': rng.choice(words), 'color': rng.getrandbits(24), 'permissions': str(rng.getrandbits(40)),
                                  'position': i, 'hoist': rng.random() < .2, 'managed': False, 'mentionable': True} for i in range(rng.randrange(5, 40))],
                       'channels': [{'id': snowflake(), 'type': rng.choice([0, 0, 0, 2, 4, 5, 15]), 'name': '-'.join(rng.choice(words) for _ in range(2)),
                                     'position': i, 'parent_id': None, 'topic': None if rng.random() < .5 else ' '.join(rng.choice(words) for _ in range(12)),
                                     'permission_overwrites': [{'id': snowflake(), 'type': 0, 'allow': '0', 'deny': str(rng.getrandbits(32))}
                                                               for _ in range(rng.randrange(0, 4))],
                                     'last_message_id': snowflake(), 'rate_limit_per_user': 0, 'nsfw': False} for i in range(rng.randrange(8, 70))],
                       'emojis': [{'id': snowflake(), 'name': rng.choice(words) + str(i), 'animated': rng.random() < .2, 'available': True}
                                  for i in range(rng.randrange(0, 50))],
                       'member_count': rng.randrange(2, 500000), 'joined_at': '2024-01-01T00:00:00.000000+00:00'})
    return {'t': 'READY', 's': 1, 'op': 0, 'd': {'v': 9, 'user': user(), 'guilds': guilds,
            'private_channels': [{'id': snowflake(), 'type': 1, 'recipients': [user()], 'last_message_id': snowflake()} for _ in range(200)],
            'users': [user() for _ in range(800)], 'session_id': '%032x' % rng.getrandbits(128)}}


def event(sequence):
    kind = rng.random()
    if kind < .45:
        return {'t': 'MESSAGE_CREATE', 's': sequence, 'op': 0, 'd': message(snowflake())}
    if kind < .8:
        return {'t': 'PRESENCE_UPDATE', 's': sequence, 'op': 0, 'd': {'user': {'id': snowflake()}, 'status': rng.choice(['online', 'idle', 'dnd']),
                                                                      'guild_id': snowflake(), 'activities': [], 'client_status': {'desktop': 'online'}}}
    return {'t': 'TYPING_START', 's': sequence, 'op': 0, 'd': {'user_id': snowflake(), 'timestamp': rng.randrange(1 << 31), 'channel_id': snowflake()}}


def png_rows(width, height, channels):
    rows = bytearray()
    for y in range(height):
        rows.append(rng.choice([0, 1, 2, 4]))
        for x in range(width):
            base = (x * 3 + y * 2) & 255
            for c in range(channels):
                rows.append((base + c * 40 + (rng.randrange(4) if not (x * 7 + y) % 5 else 0)) & 255)
    return bytes(rows)


entries = []


def add(kind, raw, encoded):
    entries.append(struct.pack('<III', len(raw), len(encoded), kind) + raw + encoded)


def compress_zlib(raw, level=6):
    return zlib.compress(raw, level)


def compress_gzip(raw, level=6):
    encoder = zlib.compressobj(level, zlib.DEFLATED, 31)
    return encoder.compress(raw) + encoder.flush()


history = json.dumps([message(snowflake()) for _ in range(100)], separators=(',', ':')).encode()
add(0, history, compress_gzip(history))
ready_payload = json.dumps(ready(), separators=(',', ':')).encode()[:2 * 1024 * 1024]
add(1, ready_payload, compress_zlib(ready_payload))
image = png_rows(512, 256, 4)
add(1, image, compress_zlib(image, 6))
runs = bytes(rng.choice(b'ab') for _ in range(64)) * 4096
add(1, runs, compress_zlib(runs, 9))

# Gateway-style connection: READY, then small events, every payload sync-flushed.
payloads = [json.dumps(ready(), separators=(',', ':')).encode()[:1024 * 1024]]
payloads += [json.dumps(event(i + 2), separators=(',', ':')).encode() for i in range(1500)]
encoder = zlib.compressobj(6, zlib.DEFLATED, 15)
segments = [encoder.compress(p) + encoder.flush(zlib.Z_SYNC_FLUSH) for p in payloads]
raw = b''.join(payloads)
table = struct.pack('<I', len(payloads)) + b''.join(struct.pack('<II', len(p), len(s)) for p, s in zip(payloads, segments))
add(2, raw, table + b''.join(segments))

with open(sys.argv[1], 'wb') as output:
    output.write(b''.join(entries))
print(len(entries), 'entries')

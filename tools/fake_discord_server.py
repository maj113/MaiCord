#!/usr/bin/env python3
"""A fake Discord for driving the Linux client signed in, with no account: REST and the user gateway over TLS.

    tools/fake_discord_server.py --cert CERT.pem --key KEY.pem [--port 443] [--site DIR] [--prefix /MaiCord-plugins]
                                 [--catalog-host maj113.github.io] [--log FILE]

tools/test_discord_wayland.sh points discord.com, gateway.discord.gg and the CDN names at 127.0.0.1 (a private /etc/hosts
in a private network namespace), trusts CERT through SSL_CERT_FILE and runs this server. The client then signs in with
any e-mail and password (POST /api/v9/auth/login answers with a token), opens the user gateway (wss://gateway.discord.gg,
ETF, one zstd frame per message) and gets one account, one guild with one text channel and a few messages. With --site the
plugin catalog (tools/catalog_site_server.py's job) is served on --catalog-host too, so the Plugins page works signed in.
The guild also has a voice channel and a stage; joining either answers the voice state and server updates, and the voice
server (us-qa.discord.media: the voice gateway v9 and UDP 50001) accepts the stage's transport-only session (no DAVE, which
the voice channel demands) and echoes the client's Opus packets back as another participant.

Nothing here is Discord's protocol beyond what the client reads; an unknown request answers 404 and is logged as
"<status> <method> <path>" to --log, which is how a missing route shows. Standard library only (voice needs the `cryptography`
package for AES-GCM); prints "ready".
"""
import argparse
import hashlib
import base64
import http.server
import json
import os
import re
import ssl
import struct
import sys
import threading
import time
import socket
import zlib

UID, OTHER = 100000000000000001, 100000000000000002
GID, CID, MEDIA, CAT = 200000000000000001, 300000000000000001, 300000000000000002, 300000000000000003
EMOJI_ID = 400000000000000001
VOICE_CHANNEL, STAGE_CHANNEL = 300000000000000004, 300000000000000005  # a voice channel (DAVE) and a stage (transport-only)
VOICE_HOST = "us-qa.discord.media"  # the voice server's name; the harness points it at this server
VOICE_UDP_PORT, VOICE_SSRC, VOICE_REMOTE_SSRC = 50001, 4242, 4343
EPOCH = 1420070400000
CDN = "https://cdn.discordapp.com"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def snowflake(ms: int, low: int) -> int:
    return ((ms - EPOCH) << 22) | low


NOW = int(time.time() * 1000)
START = time.time()


def png(width: int, height: int, pixel) -> bytes:
    """A truecolor PNG; pixel(x, y) -> (r, g, b)."""
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sunset(x, y):
    return (min(255, 40 + y), min(255, 60 + x // 3), max(0, 200 - y))


def blob(x, y):
    inside = (x - 24) ** 2 + (y - 24) ** 2 < 20 ** 2
    return (90, 200, 120) if inside else (30, 30, 36)


# What the CDN names serve: path -> (content type, bytes).
FILES = {}
MP4_PATH = os.path.join(ROOT, "tests", "fixtures", "mp4", "h264_aac_bframes.mp4")


def attachment(name: str, kind: str, data: bytes, **extra) -> dict:
    number = len(FILES) + 1
    path = f"/attachments/{MEDIA}/{500000000000000000 + number}/{name}"
    FILES[path] = (kind, data)
    expires, issued = int(time.time()) + 86400, int(time.time()) - 3600
    url = f"{CDN}{path}?ex={expires:x}&is={issued:x}&hm={'ab' * 32}"
    return dict({"id": 500000000000000000 + number, "filename": name, "size": len(data), "url": url,
                 "proxy_url": url.replace("cdn.discordapp.com", "media.discordapp.net"), "content_type": kind}, **extra)


SUNSET = png(480, 270, sunset)
FILES[f"/emojis/{EMOJI_ID}.png"] = ("image/png", png(48, 48, blob))
try:
    import io
    from PIL import Image
    _out = io.BytesIO()
    Image.open(io.BytesIO(FILES[f"/emojis/{EMOJI_ID}.png"][1])).save(_out, "WEBP", lossless=True)
    FILES[f"/emojis/{EMOJI_ID}.webp"] = ("image/webp", _out.getvalue())
except ImportError:  # no Pillow: the WebP URL answers with the PNG, which the client refuses
    FILES[f"/emojis/{EMOJI_ID}.webp"] = FILES[f"/emojis/{EMOJI_ID}.png"]


def reaction(name, count, me=False, emoji_id=None):
    return {"emoji": {"id": emoji_id, "name": name, "animated": False}, "count": count,
            "count_details": {"burst": 0, "normal": count}, "me": me, "me_burst": False, "burst_me": False,
            "burst_count": 0, "burst_colors": []}


MESSAGES = {CID: [], MEDIA: []}


def post(channel, author, content, **extra) -> dict:
    number = sum(len(v) for v in MESSAGES.values()) + 1
    message = {"id": snowflake(NOW - (30 - number) * 60000, number), "channel_id": channel, "author": None,
               "content": content, "timestamp": None, "edited_timestamp": None, "tts": False, "mention_everyone": False,
               "mentions": [], "mention_roles": [], "attachments": [], "embeds": [], "pinned": False, "type": 0, "flags": 0,
               "components": [], "_author_id": author}
    message.update(extra)
    MESSAGES[channel].append(message)
    return message


def seed() -> None:
    post(CID, OTHER, "Welcome to the QA guild. This server is fake: nothing leaves this machine.")
    post(CID, OTHER, "Have a look at https://example.com/page?utm_source=newsletter&id=7 for the tracking parameters.")
    post(CID, UID, "A message of my own, to see the plugin's buttons on it. Emoji: 😀 🎉 and the guild's own "
                   f"<:blobwave:{EMOJI_ID}>.")
    post(CID, OTHER, "**Bold**, *italic*, ~~struck~~, `code` and a [link](https://example.com/docs). The last message: plugins "
                     "decorate names and add buttons to messages.", reactions=[reaction("👍", 3), reaction("blobwave", 1, True, EMOJI_ID)])
    post(MEDIA, OTHER, "A picture, a clip and a file in one message:", attachments=[
        attachment("sunset.png", "image/png", SUNSET, width=480, height=270),
        attachment("clip.mp4", "video/mp4", open(MP4_PATH, "rb").read() if os.path.exists(MP4_PATH) else b"", width=64, height=48,
                   duration_secs=1.0),
        attachment("notes.txt", "text/plain", b"Some notes in a file.\n")])
    post(MEDIA, OTHER, "A link with a preview:", embeds=[{
        "type": "rich", "title": "MaiCord on GitHub", "description": "A freestanding Discord client in C++23.",
        "url": "https://example.com/maicord", "color": 5793266, "author": {"name": "example.com"},
        "fields": [{"name": "Language", "value": "C++23", "inline": True}, {"name": "Platforms", "value": "Windows, Linux", "inline": True}],
        "thumbnail": {"url": f"{CDN}/emojis/{EMOJI_ID}.png", "proxy_url": f"{CDN}/emojis/{EMOJI_ID}.png", "width": 48, "height": 48},
        "footer": {"text": "QA embed"}}], reactions=[reaction("🎉", 2, True)])
    post(MEDIA, UID, "Reply target, with a mention of the other person: <@%d>" % OTHER,
         mentions=[{"id": OTHER}])


NAMED_USERS = {}  # display name -> user id, for seed_extra's "author_name" and "mention_names"


def named_user(name: str) -> int:
    return NAMED_USERS.setdefault(name, 100000000000000100 + len(NAMED_USERS))


def seed_extra() -> None:
    """MAICORD_FAKE_EXTRA_MESSAGES=FILE: a JSON list of {"channel": "general"|"media", "author": "me"|"other", "content": str,
    "embeds": [...]} posted after the seed, for a run that needs one particular message (an embed with a given description).
    Optional: "author_name" (a user with that display name writes it), "mention_names" (display names whose users the
    message mentions: "{m0}", "{m1}"... in content become their <@id>) and "reply_to" (the index in this list of the
    message it answers)."""
    path = os.environ.get("MAICORD_FAKE_EXTRA_MESSAGES")
    posted = []
    for item in json.load(open(path, encoding="utf-8")) if path else []:
        mentioned = [named_user(name) for name in item.get("mention_names", [])]
        content = item.get("content", "")
        for number, user in enumerate(mentioned):
            content = content.replace("{m%d}" % number, "<@%d>" % user)
        author = named_user(item["author_name"]) if "author_name" in item else UID if item.get("author") == "me" else OTHER
        extra = {"mentions": [{"id": user} for user in mentioned]}
        channel = {"general": CID, "media": MEDIA}[item.get("channel", "media")]
        if "reply_to" in item:
            target = posted[item["reply_to"]]
            extra.update(type=19, message_reference={"type": 0, "message_id": str(target["id"]), "channel_id": str(channel),
                                                      "guild_id": str(GID)}, _reply_to=target)
        posted.append(post(channel, author, content, embeds=item.get("embeds", []), **extra))


seed()
seed_extra()
GATEWAYS = []  # one dispatch function per open user gateway connection


def mention(dispatch) -> None:
    """A message in #general that mentions the account, as the gateway would deliver it."""
    message = post(CID, OTHER, f"<@{UID}> are you there? This is the fake mention.", mentions=[{"id": UID}])
    message["id"] = snowflake(int(time.time() * 1000), 900)
    row = [r for r in message_rows(CID) if r["id"] == message["id"]][0]
    dispatch(0, dict(row, guild_id=GID, member={"roles": [], "joined_at": iso(NOW - 86400000), "deaf": False, "mute": False,
                                                  "flags": 0}), "MESSAGE_CREATE")


def iso(ms: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms / 1000)) + ".000000+00:00"


def user_object(user_id: int) -> dict:
    mine = user_id == UID
    named = {identifier: name for name, identifier in NAMED_USERS.items()}.get(user_id)
    return {"id": user_id, "username": named or ("qatester" if mine else "guide"), "discriminator": "0",
            "global_name": named or ("QA Tester" if mine else "Guide Bot Person"), "avatar": None, "avatar_decoration_data": None,
            "banner": None, "accent_color": None, "bot": False, "system": False, "flags": 0, "public_flags": 0,
            "premium_type": 0, "clan": None, "primary_guild": None}


def message_row(m: dict) -> dict:
    row = {k: v for k, v in m.items() if not k.startswith("_")}
    row["author"] = user_object(m["_author_id"])
    row["timestamp"] = iso(((row["id"] >> 22) + EPOCH))
    row["mentions"] = [user_object(int(u["id"])) for u in m["mentions"]]
    if "_reply_to" in m:
        row["referenced_message"] = message_row(m["_reply_to"])
    return row


def message_rows(channel: int) -> list:
    return [message_row(m) for m in MESSAGES[channel]]


def guild_object() -> dict:
    everyone = {"id": GID, "name": "@everyone", "description": None, "permissions": 1071698660929, "position": 0,
                "color": 0, "hoist": False, "managed": False, "mentionable": False, "icon": None, "unicode_emoji": None,
                "flags": 0}
    def channel(identifier, kind, name, position, parent, last):
        return {"id": identifier, "type": kind, "name": name, "position": position, "flags": 0, "parent_id": parent,
                "permission_overwrites": [], "nsfw": False, "topic": "Fake channel" if kind == 0 else None,
                "rate_limit_per_user": 0, "last_message_id": last}
    channels = [channel(CAT, 4, "Text", 0, None, None),
                channel(CID, 0, "general", 0, CAT, MESSAGES[CID][-1]["id"]),
                channel(MEDIA, 0, "media", 1, CAT, MESSAGES[MEDIA][-1]["id"]),
                dict(channel(VOICE_CHANNEL, 2, "Voice", 2, CAT, None), bitrate=64000, user_limit=0, rtc_region=None),
                dict(channel(STAGE_CHANNEL, 13, "Stage", 3, CAT, None), bitrate=64000, user_limit=0, rtc_region=None)]
    emojis = [{"id": EMOJI_ID, "name": "blobwave", "animated": False, "available": True, "managed": False,
               "require_colons": True, "roles": []}]
    return {"id": GID, "name": "QA Guild", "icon": None, "splash": None, "banner": None, "description": None,
            "owner_id": UID, "region": "us-east", "afk_channel_id": None, "afk_timeout": 300, "features": [],
            "verification_level": 0, "default_message_notifications": 0, "explicit_content_filter": 0, "mfa_level": 0,
            "system_channel_id": None, "system_channel_flags": 0, "rules_channel_id": None, "premium_tier": 0,
            "premium_subscription_count": 0, "preferred_locale": "en-US", "public_updates_channel_id": None,
            "nsfw_level": 0, "premium_progress_bar_enabled": False, "max_members": 500000, "member_count": 2,
            "joined_at": iso(NOW - 86400000), "large": False, "lazy": True, "unavailable": False, "roles": [everyone],
            "channels": channels, "threads": [], "emojis": emojis, "stickers": [], "stage_instances": [{
                "id": STAGE_CHANNEL + 1, "guild_id": GID, "channel_id": STAGE_CHANNEL, "topic": "QA stage", "privacy_level": 2,
                "discoverable_disabled": False, "guild_scheduled_event_id": None}],
            "guild_scheduled_events": [], "voice_states": [], "presences": [], "members": [], "application_command_counts": {},
            "version": 1, "hub_type": None, "latest_on_boarding_question_id": None, "safety_alerts_channel_id": None,
            "permissions": 1071698660929, "incidents_data": None, "embedded_activities": [], "home_header": None}


def quoted(value):
    """REST JSON writes ids and permission sets as strings (the gateway's ETF writes them as integers)."""
    if isinstance(value, dict):
        return {k: (str(v) if isinstance(v, int) and not isinstance(v, bool) and (k == "id" or k.endswith("_id") or k == "permissions")
                    else [str(i) for i in v] if k.endswith("_ids") and isinstance(v, list) else quoted(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [quoted(v) for v in value]
    return value


def ready_payload() -> dict:
    me = dict(user_object(UID), email="qa@example.invalid", verified=True, mfa_enabled=False, locale="en-US",
              nsfw_allowed=True, purchased_flags=0, premium=False, phone=None, bio="", desktop=True, mobile=False)
    return {"v": 9, "user": me, "users": [user_object(OTHER)], "guilds": [guild_object()], "private_channels": [],
            "relationships": [], "merged_presences": {"guilds": [[]], "friends": []}, "merged_members": [[{"user_id": UID,
            "roles": [], "nick": None, "avatar": None, "joined_at": iso(NOW - 86400000), "premium_since": None, "deaf": False,
            "mute": False, "flags": 0, "pending": False, "communication_disabled_until": None}]],
            "read_state": {"version": 1, "partial": False, "entries": []}, "user_guild_settings": {"version": 1,
            "partial": False, "entries": [{"guild_id": GID, "message_notifications": 1, "muted": False, "mute_config": None,
            "suppress_everyone": False, "suppress_roles": False, "mobile_push": True, "hide_muted_channels": False,
            "notify_highlights": 0, "flags": 0, "version": 1, "channel_overrides": []}]},
            "session_id": "qa-session-1", "session_type": "normal", "user_settings_proto": "mAYA",  # one unknown varint field
            "resume_gateway_url": "wss://gateway.discord.gg", "analytics_token": "qa", "auth_session_id_hash": "qa",
            "country_code": "US", "guild_join_requests": [], "connected_accounts": [], "consents": {"personalization":
            {"consented": False}}, "tutorial": None, "notification_settings": {"flags": 0}, "geo_ordered_rtc_regions": [],
            "game_relationships": [], "api_code_version": 1, "user_settings": {"locale": "en-US", "theme": "dark"},
            "required_action": None, "_trace": ["[\"qa\",{\"micros\":0}]"]}


# ---- ETF ------------------------------------------------------------------------------------------------------
def etf(value) -> bytes:
    return b"\x83" + _term(value)


def _atom(text: str) -> bytes:
    raw = text.encode()
    return b"\x77" + bytes([len(raw)]) + raw


def _term(value) -> bytes:
    if value is None:
        return _atom("nil")
    if value is True:
        return _atom("true")
    if value is False:
        return _atom("false")
    if isinstance(value, int):
        if 0 <= value < 256:
            return b"\x61" + bytes([value])
        if -(1 << 31) <= value < (1 << 31):
            return b"\x62" + struct.pack(">i", value)
        magnitude = abs(value)
        digits = magnitude.to_bytes((magnitude.bit_length() + 7) // 8, "little")
        return b"\x6e" + bytes([len(digits), 1 if value < 0 else 0]) + digits
    if isinstance(value, float):
        return b"\x46" + struct.pack(">d", value)
    if isinstance(value, str):
        raw = value.encode()
        return b"\x6d" + struct.pack(">I", len(raw)) + raw
    if isinstance(value, (list, tuple)):
        if not value:
            return b"\x6a"
        return b"\x6c" + struct.pack(">I", len(value)) + b"".join(_term(v) for v in value) + b"\x6a"
    if isinstance(value, dict):
        out = b"\x74" + struct.pack(">I", len(value))
        for key, item in value.items():
            out += _atom(key) + _term(item)
        return out
    raise TypeError(type(value))


def etf_decode(data: bytes):
    if data[0] != 131:
        raise ValueError("not ETF")
    value, _ = _read(data, 1)
    return value


def _read(data: bytes, at: int):
    tag = data[at]
    at += 1
    if tag == 97:
        return data[at], at + 1
    if tag == 98:
        return struct.unpack_from(">i", data, at)[0], at + 4
    if tag == 70:
        return struct.unpack_from(">d", data, at)[0], at + 8
    if tag in (100, 118):
        n = struct.unpack_from(">H", data, at)[0]
        return _atom_value(data[at + 2:at + 2 + n].decode()), at + 2 + n
    if tag in (115, 119):
        n = data[at]
        return _atom_value(data[at + 1:at + 1 + n].decode()), at + 1 + n
    if tag == 109:
        n = struct.unpack_from(">I", data, at)[0]
        return data[at + 4:at + 4 + n].decode("utf-8", "replace"), at + 4 + n
    if tag == 106:
        return [], at
    if tag == 108:
        n = struct.unpack_from(">I", data, at)[0]
        at += 4
        items = []
        for _ in range(n):
            item, at = _read(data, at)
            items.append(item)
        _, at = _read(data, at)  # tail
        return items, at
    if tag in (104, 105):
        n = data[at] if tag == 104 else struct.unpack_from(">I", data, at)[0]
        at += 1 if tag == 104 else 4
        items = []
        for _ in range(n):
            item, at = _read(data, at)
            items.append(item)
        return tuple(items), at
    if tag == 116:
        n = struct.unpack_from(">I", data, at)[0]
        at += 4
        result = {}
        for _ in range(n):
            key, at = _read(data, at)
            result[key], at = _read(data, at)
        return result, at
    if tag in (110, 111):
        n = data[at] if tag == 110 else struct.unpack_from(">I", data, at)[0]
        at += 1 if tag == 110 else 4
        sign = data[at]
        number = int.from_bytes(data[at + 1:at + 1 + n], "little")
        return (-number if sign else number), at + 1 + n
    raise ValueError(f"ETF tag {tag}")


def _atom_value(text: str):
    return {"nil": None, "null": None, "true": True, "false": False}.get(text, text)


def zstd_frame(data: bytes) -> bytes:
    """One zstd frame of raw (stored) blocks: a valid stream the client's decoder reads like any other."""
    header = b"\x28\xb5\x2f\xfd" + bytes([0xA0]) + struct.pack("<I", len(data))
    blocks = b""
    chunks = [data[i:i + 131072] for i in range(0, len(data), 131072)] or [b""]
    for number, chunk in enumerate(chunks):
        last = number == len(chunks) - 1
        blocks += struct.pack("<I", (len(chunk) << 3) | (1 if last else 0))[:3] + chunk
    return header + blocks


# ---- web socket ---------------------------------------------------------------------------------------------------
def ws_send(connection, payload: bytes, opcode: int = 2) -> None:
    head = bytes([0x80 | opcode])
    if len(payload) < 126:
        head += bytes([len(payload)])
    elif len(payload) < 65536:
        head += b"\x7e" + struct.pack(">H", len(payload))
    else:
        head += b"\x7f" + struct.pack(">Q", len(payload))
    connection.sendall(head + payload)


def ws_receive(stream):
    """(opcode, payload) of the next message, or None at the end of the connection."""
    message = b""
    opcode = 0
    while True:
        head = stream.read(2)
        if len(head) < 2:
            return None
        fin, code, masked, length = head[0] & 0x80, head[0] & 0x0F, head[1] & 0x80, head[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", stream.read(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", stream.read(8))[0]
        key = stream.read(4) if masked else b""
        payload = stream.read(length)
        if len(payload) < length:
            return None
        if masked:
            payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
        if code:
            opcode = code
        message += payload
        if code in (8, 9, 10):
            return code, payload
        if fin:
            return opcode, message


# ---- voice --------------------------------------------------------------------------------------------------------
# The voice server: the voice gateway (v9 JSON) answers the client's Identify with Ready and its SelectProtocol with a
# session description (AES-256-GCM, no DAVE), and a UDP socket answers the IP discovery and decrypts the client's RTP.
# Every Opus packet it receives is sent back, as another participant's, so the client's playback has something to play.
try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    AESGCM = None
VOICE = {"key": None, "received": 0, "bad": 0, "sent": 0, "counter": 0, "sock": None}


def log_line(text: str) -> None:
    if Handler.options.log:
        with Handler.lock, open(Handler.options.log, "a") as handle:
            handle.write(f"{text}\n")


def voice_state(dispatch, request) -> None:
    channel = request.get("channel_id")
    state = {"guild_id": GID, "channel_id": channel, "user_id": UID, "session_id": "qa-session-1", "deaf": False,
             "mute": False, "self_deaf": bool(request.get("self_deaf")), "self_mute": bool(request.get("self_mute")),
             "self_video": False, "self_stream": False, "suppress": False, "request_to_speak_timestamp": None}
    log_line(f"voice join {channel}")
    dispatch(0, state, "VOICE_STATE_UPDATE")
    if channel:
        dispatch(0, dict(state, user_id=OTHER, session_id="qa-session-2", self_mute=False, self_deaf=False), "VOICE_STATE_UPDATE")
        dispatch(0, {"token": "qa-voice-token", "guild_id": GID, "endpoint": VOICE_HOST}, "VOICE_SERVER_UPDATE")


def voice_gateway(connection, stream) -> None:
    sequence = [0]

    def send(op, data, numbered=True):
        message = {"op": op, "d": data}
        if numbered:
            sequence[0] += 1
            message["seq"] = sequence[0]
        ws_send(connection, json.dumps(message).encode(), 1)

    send(8, {"v": 9, "heartbeat_interval": 13750}, False)
    while True:
        received = ws_receive(stream)
        if received is None or received[0] == 8:
            log_line(f"voice summary received {VOICE['received']} sent {VOICE['sent']} undecryptable {VOICE['bad']}")
            return
        if received[0] != 1:
            continue
        try:
            message = json.loads(received[1])
        except ValueError:
            continue
        op, data = message.get("op"), message.get("d") or {}
        log_line(f"voice ws op {op}")
        if op == 0:
            log_line(f"voice identify dave {data.get('max_dave_protocol_version')} video {data.get('video')}")
            send(2, {"ssrc": VOICE_SSRC, "ip": "127.0.0.1", "port": VOICE_UDP_PORT, "modes": ["aead_aes256_gcm_rtpsize"],
                     "experiments": [], "streams": []})
        elif op == 3:
            send(6, {"t": data.get("t")}, False)
        elif op == 1:
            mode = (data.get("data") or {}).get("mode")
            log_line(f"voice select protocol mode {mode}")
            VOICE["key"] = os.urandom(32)
            send(4, {"mode": mode, "secret_key": list(VOICE["key"]), "audio_codec": "opus", "dave_protocol_version": 0})
            send(5, {"speaking": 1, "ssrc": VOICE_REMOTE_SSRC, "user_id": str(OTHER)})


def voice_udp(sock) -> None:
    while True:
        packet, peer = sock.recvfrom(4096)
        if len(packet) == 74 and packet[:2] == b"\x00\x01":
            log_line(f"voice udp discovery ssrc {int.from_bytes(packet[4:8], 'big')}")
            sock.sendto(b"\x00\x02\x00\x46" + packet[4:8] + b"127.0.0.1".ljust(64, b"\0") + struct.pack(">H", peer[1]), peer)
            continue
        key = VOICE["key"]
        if key is None or AESGCM is None or len(packet) < 28 or packet[0] >> 6 != 2 or 192 <= packet[1] <= 223:
            continue
        clear, extension = 12 + (packet[0] & 15) * 4, 0
        if packet[0] & 0x10:
            extension = struct.unpack(">H", packet[clear + 2:clear + 4])[0] * 4
            clear += 4
        try:
            plain = AESGCM(key).decrypt(packet[-4:] + b"\0" * 8, packet[clear:-4], packet[:clear])
        except Exception:  # noqa: BLE001
            VOICE["bad"] += 1
            log_line(f"voice rtp undecryptable packet {VOICE['bad']}")
            continue
        payload = plain[extension:]
        VOICE["received"] += 1
        if VOICE["received"] in (1, 10, 50, 200) or VOICE["received"] % 500 == 0:
            log_line(f"voice rtp received {VOICE['received']} payload type {packet[1] & 0x7f} ssrc {int.from_bytes(packet[8:12], 'big')} opus bytes {len(payload)}")
        header = bytes([0x80, packet[1] & 0x7f]) + packet[2:8] + struct.pack(">I", VOICE_REMOTE_SSRC)
        nonce = struct.pack(">I", VOICE["counter"])
        VOICE["counter"] += 1
        sock.sendto(header + AESGCM(key).encrypt(nonce + b"\0" * 8, payload, header) + nonce, peer)
        VOICE["sent"] += 1


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    options = None
    lock = threading.Lock()

    def log_message(self, format, *arguments):
        pass

    def note(self, status):
        if self.options.log:
            with self.lock, open(self.options.log, "a") as handle:
                handle.write(f"{status} {self.command} {self.path}\n")

    # -- the catalog ------------------------------------------------------------------------------------------------
    def serve_catalog(self):
        prefix = self.options.prefix.rstrip("/")
        path = self.path.split("?", 1)[0]
        if not self.options.site or not path.startswith(prefix + "/"):
            return self.reply(404, b"", "text/plain")
        relative = os.path.normpath(path[len(prefix) + 1:])
        target = os.path.join(self.options.site, relative)
        if relative.startswith("..") or not os.path.isfile(target):
            return self.reply(404, b"", "text/plain")
        with open(target, "rb") as handle:
            self.reply(200, handle.read(), "application/octet-stream")

    def reply(self, status, body, kind="application/json", extra=None):
        self.note(status)
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for name, value in (extra or {}).items():
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def json(self, status, value):
        self.reply(status, json.dumps(quoted(value)).encode())

    # -- the API ------------------------------------------------------------------------------------------------------
    def api(self):
        path = self.path.split("?", 1)[0]
        path = re.sub(r"^/api/v\d+", "", path)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if self.command == "POST" and path == "/auth/login":
            return self.json(200, {"user_id": str(UID), "token": "QA.fake.token", "user_settings": {"locale": "en-US", "theme": "dark"}})
        if self.command == "GET" and path == "/users/@me":
            return self.json(200, dict(user_object(UID), email="qa@example.invalid", verified=True, locale="en-US"))
        if self.command == "GET" and path == "/gateway":
            return self.json(200, {"url": "wss://gateway.discord.gg"})
        match = re.fullmatch(r"/channels/(\d+)/messages", path)
        if self.command == "GET" and match:
            query = dict(part.split("=", 1) for part in (self.path.split("?", 1)[1:] or [""])[0].split("&") if "=" in part)
            rows = message_rows(int(match.group(1)))
            limit = int(query.get("limit", 50))
            if "after" in query:
                rows = [r for r in rows if r["id"] > int(query["after"])][:limit]
            elif "before" in query:
                rows = [r for r in rows if r["id"] < int(query["before"])][-limit:]
            elif "around" in query:
                at = int(query["around"])
                nearest = min(range(len(rows)), key=lambda i: abs(rows[i]["id"] - at)) if rows else 0
                rows = rows[max(0, nearest - limit // 2):nearest + (limit + 1) // 2]
            else:
                rows = rows[-limit:]
            return self.json(200, list(reversed(rows)))
        if self.command == "POST" and match:
            sent = json.loads(body or b"{}")
            if self.options.log:
                with self.lock, open(self.options.log, "a") as handle:
                    handle.write(f"sent {sent.get('content', '')!r}\n")
            channel = int(match.group(1))
            message = post(channel, UID, sent.get("content", ""))
            message["id"] = snowflake(int(time.time() * 1000), len(MESSAGES[channel]) + 100)
            if sent.get("nonce"):
                message["nonce"] = sent["nonce"]
            row = [r for r in message_rows(channel) if r["id"] == message["id"]][0]
            for push in list(GATEWAYS):
                push(0, row, "MESSAGE_CREATE")
            return self.json(200, row)
        if self.command == "POST" and re.fullmatch(r"/channels/\d+/messages/\d+/ack", path):
            return self.json(200, {"token": None})
        self.json(404, {"message": "404: Not Found", "code": 0})

    def handle_any(self):
        host = (self.headers.get("Host") or "").split(":")[0]
        if host == self.options.catalog_host:
            return self.serve_catalog()
        if self.headers.get("Upgrade", "").lower() == "websocket":
            return self.gateway()
        if host.startswith("cdn.") or host.startswith("media."):
            found = FILES.get(self.path.split("?", 1)[0])
            return self.reply(200, found[1], found[0]) if found else self.reply(404, b"", "text/plain")
        self.api()

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = handle_any

    # -- the gateway ------------------------------------------------------------------------------------------------
    def gateway(self):
        host = (self.headers.get("Host") or "").split(":")[0]
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.note(101)
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        self.wfile.flush()
        if host.endswith(".discord.media"):
            return voice_gateway(self.connection, self.rfile)
        if host != "gateway.discord.gg":
            return  # the remote-auth gateway: accepted and left idle
        connection = self.connection
        sequence = [0]
        send_lock = threading.Lock()

        def dispatch(op, data=None, name=None):
            with send_lock:
                message = {"op": op, "d": data}
                if name:
                    sequence[0] += 1
                    message.update(t=name, s=sequence[0])
                try:
                    ws_send(connection, zstd_frame(etf(message)))
                except OSError:
                    pass

        dispatch(10, {"heartbeat_interval": 41250, "_trace": ["[\"qa\",{\"micros\":0}]"]})
        GATEWAYS.append(dispatch)
        try:
            self.gateway_loop(dispatch)
        finally:
            GATEWAYS.remove(dispatch)

    def gateway_loop(self, dispatch):
        while True:
            received = ws_receive(self.rfile)
            if received is None or received[0] == 8:
                return
            opcode, payload = received
            if opcode not in (1, 2):
                continue
            try:
                message = etf_decode(payload) if payload[:1] == b"\x83" else json.loads(payload)
            except Exception as error:  # noqa: BLE001
                self.note(f"bad-frame:{error}")
                continue
            op = message.get("op")
            if self.options.log:
                with self.lock, open(self.options.log, "a") as handle:
                    handle.write(f"ws op {op} at {time.time() - START:.1f}s (ws opcode {opcode})\n")
            if op in (1, 40):  # the user gateway's heartbeat is op 40
                dispatch(11)
            elif op == 6:
                dispatch(0, {"_trace": ["[\"qa\",{\"micros\":0}]"]}, "RESUMED")
            elif op == 4:  # a voice state update: join (or leave) the voice channel
                voice_state(dispatch, message.get("d") or {})
            elif op == 2:
                dispatch(0, ready_payload(), "READY")
                dispatch(0, {"merged_members": [[]], "merged_presences": {"guilds": [[]], "friends": []}, "lazy_private_channels": [],
                             "guilds": [{"id": GID, "voice_states": []}]}, "READY_SUPPLEMENTAL")
                if self.options.mention_after:
                    timer = threading.Timer(self.options.mention_after, mention, (dispatch,))
                    timer.daemon = True
                    timer.start()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--site")
    parser.add_argument("--prefix", default="/MaiCord-plugins")
    parser.add_argument("--catalog-host", default="maj113.github.io")
    parser.add_argument("--log")
    parser.add_argument("--mention-after", type=float, default=0, help="seconds after sign-in until a message mentions the account")
    Handler.options = parser.parse_args()
    if Handler.options.site:
        Handler.options.site = os.path.abspath(Handler.options.site)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", Handler.options.port), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(Handler.options.cert, Handler.options.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    voice_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    voice_socket.bind(("127.0.0.1", VOICE_UDP_PORT))
    threading.Thread(target=voice_udp, args=(voice_socket,), daemon=True).start()
    print("ready", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())

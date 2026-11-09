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
server (us-qa.discord.media: the voice gateway v9 and a UDP port its Ready names) accepts the stage's transport-only session
(no DAVE, which the voice channel demands) and echoes the client's Opus packets back as another participant. Go Live (ops
18-22) has its own stream server (us-qa-stream.discord.media: voice gateway v9 with DAVE, and its own UDP port) relaying a
stream to its viewers. Both UDP ports are ephemeral (the client takes them from Ready), so several fakes run at once.
On Windows a client built with tools/build.ps1 -TestHooks needs no hosts file: MAICORD_TEST_SERVER=127.0.0.1:PORT answers
every name lookup with this server (--port PORT) and MAICORD_TEST_CA_FILE=CERT.pem is the only certificate trust (CERT must
cover discord.com, gateway.discord.gg, cdn.discordapp.com and media.discordapp.net); see CONTRIBUTING.md.

Nothing here is Discord's protocol beyond what the client reads; an unknown request answers 404 and is logged as
"<status> <method> <path>" to --log, which is how a missing route shows. Standard library only (voice needs the `cryptography`
package for AES-GCM); prints "ready". MAICORD_FAKE_WORLD=DIR swaps the QA guild for a fuller made-up world (several guilds,
people with avatars and profiles, a member list, direct messages): see tools/fake_discord_world.py.
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
import gzip
import urllib.parse

import fake_discord_world

UID, OTHER = 100000000000000001, 100000000000000002
GID, CID, MEDIA, CAT = 200000000000000001, 300000000000000001, 300000000000000002, 300000000000000003
EMOJI_ID = 400000000000000001
VOICE_CHANNEL, STAGE_CHANNEL = 300000000000000004, 300000000000000005  # a voice channel (DAVE) and a stage (transport-only)
VOICE_HOST = "us-qa.discord.media"  # the voice server's name; the harness points it at this server
VOICE_UDP_PORT, VOICE_SSRC, VOICE_REMOTE_SSRC = 0, 4242, 4343  # the port is bound at start (main)
EPOCH = 1420070400000
CDN = "https://cdn.discordapp.com"
# Discord's detectable lists as the client fetches them (game detection: src/app/account/detectable_list.cpp): the
# CDN's games-v1.json and GET /games/detectable/exclusions, each answered with an ETag and 304 when it matches.
DETECTABLE_GAMES = json.dumps([
    {"id": "1402418696126992445", "name": "Starfall Tactics", "executables": [
        {"name": "starfall/starfall.exe", "os": "win32"}, {"name": "starfall", "os": "linux"},
        {"name": "starfall launcher.exe", "os": "win32", "is_launcher": True}]},
    {"id": "1402418696126992446", "name": "Lumen Drift", "executables": [
        {"name": ">java", "os": "linux", "arguments": "lumen.main"}, {"name": "lumen.app", "os": "darwin"}]},
], separators=(",", ":")).encode()
DETECTABLE_EXCLUSIONS = json.dumps({"executables": ["launcher.exe", "unitycrashhandler64.exe"],
                                    "patterns": ["vcredist.*\\.exe$"]}, separators=(",", ":")).encode()
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

# The CDN's six default avatars (embed/avatars/N.png): the fake serves flat colours of its own, not Discord's art.
for _index in range(6):
    _tint = (60 + 30 * _index, 160 - 20 * _index, 220 - 25 * _index)
    FILES[f"/embed/avatars/{_index}.png"] = ("image/png", png(64, 64, lambda x, y, c=_tint: c))

# The QA guild's server profile (GET/PATCH /guilds/{GID}/profile), the fields Server Settings reads and writes
# (Userdoccers: Discovery > Guild Profile Structure). The guild has GUILD_TAGS, so its Server Tag page is open. A tag
# save makes a badge picture for the CDN (clan-badges/{guild}/{badge_hash}.png: a flat square of the primary colour).
PROFILE = {"id": GID, "name": "QA Guild", "icon_hash": None, "member_count": 2, "online_count": 1, "description": "",
           "brand_color_primary": None, "banner_hash": None, "game_application_ids": [], "game_activity": {},
           "tag": None, "badge": 6, "badge_color_primary": None, "badge_color_secondary": None, "badge_hash": None,
           "traits": [], "features": [], "visibility": 1, "custom_banner_hash": None, "premium_subscription_count": 0,
           "premium_tier": 0}
PRIMARY_GUILD = {"value": None}  # the account's primary_guild once it adopts the tag (PUT /users/@me/clan)
# The QA guild's features: GUILD_TAGS unless MAICORD_FAKE_GUILD_FEATURES lists others (comma separated; empty for none).
GUILD_FEATURES = [name for name in os.environ.get("MAICORD_FAKE_GUILD_FEATURES", "GUILD_TAGS").split(",") if name]
# MAICORD_FAKE_PROFILE_TRAITS: the profile's stored traits as a JSON array (the Server Profile probe's "Chilling" and
# "Gaming", which name their emoji as the official client writes them).
if os.environ.get("MAICORD_FAKE_PROFILE_TRAITS"):
    PROFILE["traits"] = json.loads(os.environ["MAICORD_FAKE_PROFILE_TRAITS"])
# Games the Server Profile page finds (GET /games/autocomplete, GET /games?game_ids=) and the QA guild's most played
# (GET /guilds/{GID}/top-games); each cover is a flat picture on the CDN's app-icons path.
GAMES = {
    1402418696126992445: {"name": "Starfall Tactics", "cover": "c0ffee00c0ffee00c0ffee00c0ffee00", "tint": (90, 60, 160)},
    1402418491272986635: {"name": "Starlight Racers", "cover": "c0ffee01c0ffee01c0ffee01c0ffee01", "tint": (40, 140, 90)},
    1158877933042143272: {"name": "Moss Garden", "cover": "c0ffee02c0ffee02c0ffee02c0ffee02", "tint": (160, 120, 40)},
}
TOP_GAMES = [{"game_application_id": str(1158877933042143272), "activity_level": 2, "activity_score": 80},
             {"game_application_id": str(1402418491272986635), "activity_level": 1, "activity_score": 20}]
for _id, _game in GAMES.items():
    FILES[f"/app-icons/{_id}/{_game['cover']}.png"] = ("image/png", png(48, 64, lambda x, y, t=_game["tint"]: t))


def game_object(game_id: int) -> dict:
    """A game as GET /games returns it: the documented hashes and the newer media."""
    game = GAMES[game_id]
    return {"id": str(game_id), "name": game["name"], "icon_hash": game["cover"], "cover_image_hash": game["cover"],
            "media": {"cover": {"type": "hash", "value": game["cover"]}}, "aliases": [], "executables": []}


def game_route(command: str, path: str, raw_query: str):
    """The Server Profile's game reads, or None for any other request."""
    if command != "GET":
        return None
    query = urllib.parse.parse_qs(raw_query)
    if path == f"/guilds/{GID}/top-games":
        return {"top_games": TOP_GAMES}
    if path == "/games/autocomplete":
        typed = query.get("q", [""])[0]
        return [{"id": str(game_id), "name": game["name"], "icon": game["cover"]} for game_id, game in GAMES.items()
                if typed and typed in game["name"].lower()]
    if path == "/games":
        return [game_object(int(i)) for i in query.get("game_ids", []) if int(i) in GAMES]
    return None

# The game profile's details of one game (Userdoccers: Games > Game Object), made up for the fake: the profile reads the
# top-level fields first, so the supplemental data's summary must not show.
GAME_DETAILS = {
    1402418696126992445: {
        "description": "A made-up tactics game for the fake server: command a fleet across a starfield, one turn at a "
                       "time.",
        "genres": [48, 53, 67], "platforms": [0, 6, 7, 1],
        "companies": [{"name": "QA Publishing", "roles": [1]}, {"name": "Fake Studio", "roles": [2]}],
        "first_release_date": "2024-03-14T00:00:00+00:00",
        "websites": [{"url": "https://example.com/starfall", "category": 1},
                     {"url": "https://example.com/starfall/wiki", "category": 2},
                     {"url": "https://example.com/starfall/x", "category": 5}],
        "supplemental_game_data": {"application_id": "1402418696126992445", "name": "Starfall Tactics",
                                   "summary": "Supplemental summary.", "publisher_names": ["Other Publisher"]},
    },
}
_game_object = game_object


def game_object(game_id: int) -> dict:  # noqa: F811 - the same game with its profile details
    return dict(_game_object(game_id), **GAME_DETAILS.get(game_id, {}))


# MAICORD_FAKE_GAME_FRIEND=1: the other user is a friend playing Starfall Tactics (the game profile's Friends Who Play).
GAME_FRIEND = bool(os.environ.get("MAICORD_FAKE_GAME_FRIEND"))


def profile_patch(body: dict) -> dict:
    """Applies a PATCH /guilds/{GID}/profile body; a tag field change remakes the badge picture."""
    for key in ("name", "description", "brand_color_primary", "game_application_ids", "traits", "visibility", "tag",
                "badge", "badge_color_primary", "badge_color_secondary"):
        if key in body:
            PROFILE[key] = body[key]
    if any(key in body for key in ("tag", "badge", "badge_color_primary", "badge_color_secondary")):
        if PROFILE["tag"]:
            key = f"{PROFILE['tag']}{PROFILE['badge']}{PROFILE['badge_color_primary']}{PROFILE['badge_color_secondary']}"
            PROFILE["badge_hash"] = hashlib.md5(key.encode()).hexdigest()
            color = PROFILE["badge_color_primary"] or "#7fb134"
            tint = tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
            FILES[f"/clan-badges/{GID}/{PROFILE['badge_hash']}.png"] = ("image/png", png(32, 32, lambda x, y: tint))
        else:
            PROFILE["badge_hash"] = None
    return PROFILE


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


def broadcast(op, data=None, name=None) -> None:
    for push in list(GATEWAYS):
        push(op, data, name)


# MAICORD_FAKE_WORLD=DIR: a fuller made-up world (tools/fake_discord_world.py) answers instead of the QA guild.
WORLD = fake_discord_world.load(lambda text: log_line(text))


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
            "premium_type": 0, "clan": None, "primary_guild": PRIMARY_GUILD["value"] if mine else None}


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


# MAICORD_FAKE_ROLE=1 adds a "QA Role" under @everyone, which the Roles page edits through PATCH /guilds/{GID}/roles/{id}.
EXTRA_ROLE = {"id": GID + 776, "name": "QA Role", "description": None, "permissions": 0, "position": 1, "color": 0,
              "hoist": False, "managed": False, "mentionable": False, "icon": None, "unicode_emoji": None,
              "flags": 0} if os.environ.get("MAICORD_FAKE_ROLE") else None


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
            "owner_id": UID, "region": "us-east", "afk_channel_id": None, "afk_timeout": 300, "features": GUILD_FEATURES,
            "verification_level": 0, "default_message_notifications": 0, "explicit_content_filter": 0, "mfa_level": 0,
            "system_channel_id": None, "system_channel_flags": 0, "rules_channel_id": None, "premium_tier": 0,
            "premium_subscription_count": 0, "preferred_locale": "en-US", "public_updates_channel_id": None,
            "nsfw_level": 0, "premium_progress_bar_enabled": False, "max_members": 500000, "member_count": 2,
            "joined_at": iso(NOW - 86400000), "large": False, "lazy": True, "unavailable": False,
            "roles": [everyone] + ([dict(EXTRA_ROLE)] if EXTRA_ROLE else []),
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
    return {"v": 9, "user": me, "users": [user_object(OTHER)], "guilds": [guild_state()], "private_channels": [],
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


# MAICORD_FAKE_MESSAGE_REQUEST=1: a stranger's DM waits as a message request (the Message Requests page); accepting it
# (PUT /channels/{id}/recipients/@me) or ignoring it (DELETE) answers 204 and pushes the channel's new state.
MESSAGE_REQUEST = bool(os.environ.get("MAICORD_FAKE_MESSAGE_REQUEST"))
STRANGER = named_user("Pat Stranger") if MESSAGE_REQUEST else 0
REQUEST_CHANNEL = 300000000000000077

_ready_payload = ready_payload


def ready_payload() -> dict:  # noqa: F811 - with MAICORD_FAKE_GAME_FRIEND, the other user plays Starfall Tactics
    payload = _ready_payload()
    if GAME_FRIEND:
        payload["relationships"] = [{"id": OTHER, "type": 1, "nickname": None, "user_id": OTHER,
                                     "since": iso(NOW - 86400000 * 300)}]
        payload["merged_presences"]["friends"] = [{
            "user": {"id": OTHER}, "user_id": OTHER, "status": "online", "client_status": {"desktop": "online"},
            "activities": [{"type": 0, "name": "Starfall Tactics", "application_id": 1402418696126992445,
                            "created_at": NOW - 600000, "timestamps": {"start": NOW - 600000}, "id": "game-0"}]}]
    if MESSAGE_REQUEST:
        payload["users"].append(user_object(STRANGER))
        payload["private_channels"].append({"id": REQUEST_CHANNEL, "type": 1, "recipient_ids": [STRANGER],
                                            "last_message_id": (NOW - 3600000 - EPOCH) << 22, "is_message_request": True,
                                            "is_message_request_timestamp": iso(NOW - 3600000), "is_spam": False})
    return payload


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
        with Handler.lock, open(Handler.options.log, "a", encoding="utf-8") as handle:
            handle.write(f"{text}\n")


def voice_state(dispatch, request) -> None:
    channel = request.get("channel_id")
    state = {"guild_id": GID, "channel_id": channel, "user_id": UID, "session_id": "qa-session-1", "deaf": False,
             "mute": False, "self_deaf": bool(request.get("self_deaf")), "self_mute": bool(request.get("self_mute")),
             "self_video": False, "self_stream": False, "suppress": False, "request_to_speak_timestamp": None}
    log_line(f"voice join {channel}")
    GO_LIVE["voice_state"] = state
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


# ---- Go Live ----------------------------------------------------------------------------------------------------
# The main Gateway's stream commands (18 STREAM_CREATE, 19 STREAM_DELETE, 20 STREAM_WATCH, 22 STREAM_SET_PAUSED) answer
# with the dispatches Discord sends, and the stream server (STREAM_HOST, the voice gateway v9 and UDP STREAM_UDP_PORT)
# carries a stream between its sender and viewers: Ready assigns each Identify stream a video and RTX SSRC, Video (op
# 12) and Speaking (op 5) are relayed, and the media is forwarded as an SFU forwards it (every connection of a server
# gets the same transport key, so datagrams pass unchanged from the sender to the viewers and the viewers' RTCP back).
# DAVE runs as the voice server runs it: the server is the MLS external sender (P-256, below), proposes each joiner's
# key package to the group, relays the winning commit (op 29) and welcome (op 30), and executes the transition (op
# 22) once everyone in it is ready (op 23). Standard library only. Log lines (--log): "stream create|delete|watch|
# paused ...", "go live identify user U video V", "go live codec C", "go live video ssrc S active A", "go live dave
# epoch N members M", "go live rtp video|audio packets N", "go live rtcp packets N". MAICORD_FAKE_STREAM_CODEC
# (H264, H265 or AV1) picks the codec when the sender offers it; MAICORD_FAKE_STREAM_VIEWER_AFTER=SECONDS (default 2,
# 0: never) adds the other user as a viewer of the account's own stream.
STREAM_HOST, STREAM_UDP_PORT, STREAM_SERVER_ID = "us-qa-stream.discord.media", 0, 500000000000000001  # port: main
GO_LIVE = {"voice_state": None, "servers": {}, "lock": threading.RLock(), "paused": {}}

# NIST P-256 and ECDSA with SHA-256 (k is random).
_P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
_N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
_G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
      0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)


def _p256_add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % _P == 0:
        return None
    if a == b:
        slope = 3 * (a[0] * a[0] - 1) * pow(2 * a[1], -1, _P) % _P
    else:
        slope = (b[1] - a[1]) * pow(b[0] - a[0], -1, _P) % _P
    x = (slope * slope - a[0] - b[0]) % _P
    return x, (slope * (a[0] - x) - a[1]) % _P


def _p256_multiply(k, point):
    result = None
    while k:
        if k & 1:
            result = _p256_add(result, point)
        point = _p256_add(point, point)
        k >>= 1
    return result


def _der_integer(value):
    raw = value.to_bytes(32, "big").lstrip(b"\0") or b"\0"
    if raw[0] & 0x80:
        raw = b"\0" + raw
    return b"\x02" + bytes([len(raw)]) + raw


def _ecdsa_sign(secret, data):
    digest = int.from_bytes(hashlib.sha256(data).digest(), "big")
    while True:
        k = int.from_bytes(os.urandom(32), "big") % (_N - 1) + 1
        r = _p256_multiply(k, _G)[0] % _N
        s = pow(k, -1, _N) * (digest + r * secret) % _N
        if r and s:
            body = _der_integer(r) + _der_integer(s)
            return b"\x30" + bytes([len(body)]) + body


def _mls_varint(value):
    if value < 64:
        return bytes([value])
    if value < 16384:
        return struct.pack(">H", 0x4000 | value)
    return struct.pack(">I", 0x80000000 | value)


def _mls_vector(data):
    return _mls_varint(len(data)) + data


def _mls_vector_end(data, at):
    width = 1 << (data[at] >> 6)
    return at + width + int.from_bytes(bytes([data[at] & 0x3f]) + data[at + 1:at + width], "big")


def _is_welcome(data, at):
    """Whether data[at:] is exactly one Welcome as DAVE sends it after the commit (no MLSMessage framing): cipher
    suite 2, secrets<V>, encrypted_group_info<V>."""
    try:
        if data[at:at + 2] != b"\x00\x02":
            return False
        return _mls_vector_end(data, _mls_vector_end(data, at + 2)) == len(data)
    except IndexError:
        return False


class StreamConnection:
    def __init__(self, connection):
        self.connection, self.sequence, self.send_lock = connection, 0, threading.Lock()
        self.user_id = self.ssrc = 0
        self.addr = self.video = self.speaking = None

    def send(self, op, data, numbered=True):
        with self.send_lock:
            message = {"op": op, "d": data}
            if numbered:
                self.sequence += 1
                message["seq"] = self.sequence
            try:
                ws_send(self.connection, json.dumps(message).encode(), 1)
            except OSError:
                pass

    def send_binary(self, op, payload):
        with self.send_lock:
            self.sequence += 1
            try:
                ws_send(self.connection, struct.pack(">HB", self.sequence, op) + payload, 2)
            except OSError:
                pass


class StreamServer:
    def __init__(self):
        self.key, self.connections, self.group, self.joining = os.urandom(32), [], [], []
        self.secret = int.from_bytes(os.urandom(32), "big") % (_N - 1) + 1
        public = _p256_multiply(self.secret, _G)
        self.public = b"\x04" + public[0].to_bytes(32, "big") + public[1].to_bytes(32, "big")
        self.group_id, self.epoch, self.transition, self.pending, self.codec = 0, 0, 0, None, None
        self.next_ssrc = 7000

    def external_sender(self):
        return _mls_vector(self.public) + b"\x00\x01" + _mls_vector(b"stream")

    def add_proposal(self, key_package):
        """An external-sender PublicMessage proposing Add(key_package), signed over FramedContentTBS."""
        content = (b"\x00\x01\x00\x01" + _mls_vector(self.group_id.to_bytes(8, "big")) + struct.pack(">Q", self.epoch) +
                   b"\x02\x00\x00\x00\x00" + _mls_varint(0) + b"\x02\x00\x01" + key_package)
        signed = _mls_vector(b"MLS 1.0 FramedContentTBS") + _mls_vector(content)
        return content + _mls_vector(_ecdsa_sign(self.secret, signed))

    def others(self, connection):
        return [c for c in self.connections if c is not connection and c.user_id]


def stream_command(dispatch, op, data) -> None:
    """The main Gateway's ops 18-22 for the account's own stream and the ones it watches."""
    state = GO_LIVE["voice_state"]
    if op == 18:
        guild, channel = data.get("guild_id"), data.get("channel_id")
        key = f"guild:{guild}:{channel}:{UID}" if data.get("type") == "guild" else f"call:{channel}:{UID}"
    else:
        key = data.get("stream_key") or ""
    action = {18: "create", 19: "delete", 20: "watch"}.get(op, f"paused {str(bool(data.get('paused'))).lower()}")
    log_line(f"stream {action} {key}")
    created = {"stream_key": key, "rtc_server_id": STREAM_SERVER_ID, "region": "us-qa", "viewer_ids": [],
               "paused": False}
    server = {"token": "qa-stream-token", "stream_key": key, "guild_id": GID if key.startswith("guild:") else None,
              "endpoint": STREAM_HOST}
    if op == 18:
        GO_LIVE["paused"][key] = False
        if state:
            dispatch(0, dict(state, self_stream=True), "VOICE_STATE_UPDATE")
        dispatch(0, created, "STREAM_CREATE")
        dispatch(0, server, "STREAM_SERVER_UPDATE")
        after = float(os.environ.get("MAICORD_FAKE_STREAM_VIEWER_AFTER", "2"))
        if after > 0:
            def viewer():
                if key in GO_LIVE["paused"]:
                    dispatch(0, {"stream_key": key, "region": "us-qa", "viewer_ids": [OTHER],
                                 "paused": GO_LIVE["paused"][key]}, "STREAM_UPDATE")
            timer = threading.Timer(after, viewer)
            timer.daemon = True
            timer.start()
    elif op == 19:
        GO_LIVE["paused"].pop(key, None)
        dispatch(0, {"stream_key": key, "reason": "user_requested", "unavailable": False}, "STREAM_DELETE")
        if state and key.endswith(f":{UID}"):
            dispatch(0, dict(state, self_stream=False), "VOICE_STATE_UPDATE")
    elif op == 20:
        dispatch(0, dict(created, viewer_ids=[UID]), "STREAM_CREATE")
        dispatch(0, server, "STREAM_SERVER_UPDATE")
    elif op == 22 and key in GO_LIVE["paused"]:
        GO_LIVE["paused"][key] = bool(data.get("paused"))
        dispatch(0, {"stream_key": key, "region": "us-qa", "viewer_ids": [], "paused": GO_LIVE["paused"][key]},
                 "STREAM_UPDATE")


def _stream_codec(codecs):
    offered = [c.get("name") for c in codecs if c.get("type") == "video" and c.get("encode", True)]
    wanted = os.environ.get("MAICORD_FAKE_STREAM_CODEC", "")
    return wanted if wanted in offered else (offered[0] if offered else None)


def _stream_message(server, me, op, data) -> None:
    if op == 0:
        server.group_id = int(data.get("channel_id") or 0)
        me.user_id = int(data.get("user_id") or 0)
        me.ssrc = server.next_ssrc
        streams = []
        for wanted in data.get("streams") or []:
            ssrc = server.next_ssrc + 1 + 2 * len(streams)
            streams.append({"type": "video", "ssrc": ssrc, "rtx_ssrc": ssrc + 1, "rid": wanted.get("rid", "100"),
                            "quality": wanted.get("quality", 100), "active": False})
        server.next_ssrc += 16
        log_line(f"go live identify user {me.user_id} video {str(bool(data.get('video'))).lower()} "
                 f"dave {data.get('max_dave_protocol_version')}")
        me.send(2, {"ssrc": me.ssrc, "ip": "127.0.0.1", "port": STREAM_UDP_PORT, "modes": ["aead_aes256_gcm_rtpsize"],
                    "experiments": [], "streams": streams})
        for other in server.others(me):
            other.send(11, {"user_ids": [str(me.user_id)]})
    elif op == 1:
        codec = _stream_codec(data.get("codecs") or [])
        if codec and server.codec is None:
            server.codec = codec
            log_line(f"go live codec {codec}")
        me.send(4, {"mode": (data.get("data") or {}).get("mode"), "secret_key": list(server.key),
                    "audio_codec": "opus", "video_codec": server.codec or "H264", "media_session_id": "qa-media",
                    "dave_protocol_version": 1})
        me.send_binary(25, server.external_sender())
        others = server.others(me)
        if others:
            me.send(11, {"user_ids": [str(o.user_id) for o in others]})
        for other in others:
            if other.speaking:
                me.send(5, other.speaking)
            if other.video:
                me.send(12, other.video)
    elif op == 3:
        me.send(6, {"t": data.get("t")}, False)
    elif op == 5:
        me.speaking = {"speaking": data.get("speaking", 0), "ssrc": data.get("ssrc", me.ssrc),
                       "user_id": str(me.user_id)}
        for other in server.others(me):
            other.send(5, me.speaking)
    elif op == 12:
        me.video = dict(data, user_id=str(me.user_id))
        active = any(s.get("active") for s in data.get("streams") or [])
        log_line(f"go live video ssrc {data.get('video_ssrc')} active {str(active).lower()}")
        for other in server.others(me):
            other.send(12, me.video)
    elif op == 23 and server.pending and data.get("transition_id") == server.pending["id"]:
        server.pending["ready"].add(me)
        if server.pending["ready"] >= set(server.pending["members"]):
            for member in server.pending["members"]:
                member.send(22, {"transition_id": server.pending["id"]})
            server.group, server.joining = list(server.pending["members"]), []
            server.epoch += 1
            server.pending = None
            log_line(f"go live dave epoch {server.epoch} members {len(server.group)}")
    elif op == 31:
        log_line(f"go live dave invalid commit welcome from user {me.user_id}")


def _stream_binary(server, me, op, payload) -> None:
    if op == 26:  # A key package: the first member's group is its own; later ones are proposed to the group.
        if not server.group:
            server.group = [me]
        elif me not in server.group and me not in server.joining and not server.pending:
            server.joining.append(me)
            proposal = server.add_proposal(payload)
            for member in server.group:
                member.send_binary(27, b"\x00" + _mls_vector(proposal))
    elif op == 28 and server.pending is None and server.joining:  # The first commit wins.
        split = next((at for at in range(len(payload)) if _is_welcome(payload, at)), len(payload))
        log_line(f"go live dave commit {split} bytes, welcome {len(payload) - split} bytes")
        server.transition = server.transition % 65535 + 1
        server.pending = {"id": server.transition, "members": server.group + server.joining, "ready": set()}
        for member in server.group:
            member.send_binary(29, struct.pack(">H", server.transition) + payload[:split])
        for joiner in server.joining:
            joiner.send_binary(30, struct.pack(">H", server.transition) + payload[split:])


def stream_gateway(connection, stream) -> None:
    with GO_LIVE["lock"]:
        server = GO_LIVE["servers"].setdefault(STREAM_SERVER_ID, StreamServer())
        me = StreamConnection(connection)
        server.connections.append(me)
    me.send(8, {"v": 9, "heartbeat_interval": 13750}, False)
    try:
        while True:
            received = ws_receive(stream)
            if received is None or received[0] == 8:
                return
            with GO_LIVE["lock"]:
                if received[0] == 2 and received[1]:
                    _stream_binary(server, me, received[1][0], received[1][1:])
                elif received[0] == 1:
                    try:
                        message = json.loads(received[1])
                    except ValueError:
                        continue
                    _stream_message(server, me, message.get("op"), message.get("d") or {})
    finally:
        with GO_LIVE["lock"]:
            server.connections.remove(me)
            for other in server.others(me):
                other.send(13, {"user_id": str(me.user_id)})
            if me in server.group:
                server.group.remove(me)
            if not server.connections:
                del GO_LIVE["servers"][STREAM_SERVER_ID]


def stream_udp(sock) -> None:
    counts = {"rtp video": 0, "rtp audio": 0, "rtcp": 0}
    while True:
        packet, peer = sock.recvfrom(4096)
        with GO_LIVE["lock"]:
            server = GO_LIVE["servers"].get(STREAM_SERVER_ID)
            if server is None:
                continue
            if len(packet) == 74 and packet[:2] == b"\x00\x01":
                ssrc = int.from_bytes(packet[4:8], "big")
                for connection in server.connections:
                    if connection.ssrc == ssrc:
                        connection.addr = peer
                sock.sendto(b"\x00\x02\x00\x46" + packet[4:8] + b"127.0.0.1".ljust(64, b"\0") +
                            struct.pack(">H", peer[1]), peer)
                continue
            source = next((c for c in server.connections if c.addr == peer), None)
            if source is None or len(packet) < 12 or packet[0] >> 6 != 2:
                continue
            kind = "rtcp" if 192 <= packet[1] <= 223 else "rtp audio" if packet[1] & 0x7f == 120 else "rtp video"
            counts[kind] += 1
            if counts[kind] in (1, 10, 100, 1000) or counts[kind] % 5000 == 0:
                log_line(f"go live {kind} packets {counts[kind]}")
            for other in server.others(source):
                if other.addr:
                    sock.sendto(packet, other.addr)


# ---- Server Settings: apps, webhooks, command permissions and AutoMod rules ----------------------------------------
# One app (a bot added by the account) with app-wide command permissions and two commands, one incoming webhook and
# one followed channel in #general, and the mention spam rule. Writes change this state and push the Gateway events
# Discord pushes for them; the bodies are logged by Handler.api. With MAICORD_FAKE_AUTOMATION_EXTERNAL=1 another
# moderator changes each list three seconds after the client first reads it (a webhook, a rule, a command's
# permissions, the integrations), so a probe can tell that the client follows those events.
APP_ID, BOT_ID = 600000000000000001, 600000000000000002
AUTOMATION = {
    "next": 700000000000000000,
    "integrations": [{"id": "650000000000000001", "name": "QA Bot", "type": "discord", "enabled": True,
                      "account": {"id": str(BOT_ID), "name": "QA Bot"}, "scopes": ["bot", "applications.commands"],
                      "application": {"id": str(APP_ID), "name": "QA Bot", "icon": None, "description": "",
                                      "bot": {"id": str(BOT_ID), "username": "qabot", "bot": True}},
                      "user": {"id": str(UID), "username": "qatester", "global_name": None}}],
    "webhooks": [{"id": "660000000000000001", "type": 1, "guild_id": str(GID), "channel_id": str(CID),
                  "name": "Captain Hook", "avatar": None, "application_id": None, "token": "qa-webhook-token",
                  "user": {"id": str(UID), "username": "qatester", "global_name": None}},
                 {"id": "660000000000000002", "type": 2, "guild_id": str(GID), "channel_id": str(CID),
                  "name": "Announcements", "avatar": None, "application_id": None,
                  "source_guild": {"id": "210000000000000001", "name": "Upstream", "icon": None},
                  "source_channel": {"id": "310000000000000001", "name": "news"},
                  "user": {"id": str(UID), "username": "qatester", "global_name": None}}],
    "commands": [{"id": "600000000000000011", "application_id": str(APP_ID), "version": "1", "type": 1, "name": "ping",
                  "description": "Check that the bot answers", "options": []},
                 {"id": "600000000000000012", "application_id": str(APP_ID), "version": "1", "type": 2, "name": "Wave",
                  "description": ""}],
    "permissions": {str(APP_ID): [{"id": str(GID - 1), "type": 3, "permission": True}]},
    "external": set(),
    "integration_lists": 0,
    "rules": [{"id": "670000000000000001", "guild_id": str(GID), "name": "Block Mention Spam",
               "creator_id": str(UID), "event_type": 1, "trigger_type": 5,
               "trigger_metadata": {"mention_total_limit": 20, "mention_raid_protection_enabled": False},
               "actions": [{"type": 1, "metadata": {}}], "enabled": True, "exempt_roles": [], "exempt_channels": [],
               "position": 0}],
}


def automation_external(kind: str) -> None:
    """Another moderator's change, three seconds after the client first reads `kind` (MAICORD_FAKE_AUTOMATION_EXTERNAL)."""
    state = AUTOMATION
    if os.environ.get("MAICORD_FAKE_AUTOMATION_EXTERNAL") != "1" or kind in state["external"]:
        return
    state["external"].add(kind)

    def change() -> None:
        if kind == "webhooks":
            state["webhooks"].append({"id": "660000000000000003", "type": 1, "guild_id": str(GID), "channel_id": str(CID),
                                      "name": "Elsewhere Hook", "avatar": None, "application_id": None,
                                      "token": "qa-elsewhere-token",
                                      "user": {"id": str(OTHER), "username": "othermod", "global_name": None}})
            broadcast(0, {"guild_id": str(GID), "channel_id": str(CID)}, "WEBHOOKS_UPDATE")
        elif kind == "rules":
            rule = {"id": "670000000000000009", "guild_id": str(GID), "name": "Elsewhere Words", "creator_id": str(OTHER),
                    "event_type": 1, "trigger_type": 1,
                    "trigger_metadata": {"keyword_filter": ["spoiler"], "regex_patterns": [], "allow_list": []},
                    "actions": [{"type": 1, "metadata": {}}], "enabled": True, "exempt_roles": [],
                    "exempt_channels": [], "position": 1}
            state["rules"].append(rule)
            broadcast(0, rule, "AUTO_MODERATION_RULE_CREATE")
        elif kind == "permissions":
            command = state["commands"][0]["id"]
            state["permissions"][command] = [{"id": str(GID), "type": 1, "permission": False}]
            broadcast(0, {"id": command, "application_id": str(APP_ID), "guild_id": str(GID),
                          "permissions": state["permissions"][command]}, "APPLICATION_COMMAND_PERMISSIONS_UPDATE")
        elif kind == "integrations":
            broadcast(0, {"guild_id": str(GID)}, "GUILD_INTEGRATIONS_UPDATE")
        log_line(f"automation external change {kind}")
    threading.Timer(3.0, change).start()


def automation_api(command: str, path: str, body: bytes):
    """(status, value) for the Integrations and AutoMod pages' routes, or None for any other request."""
    state, guild = AUTOMATION, f"/guilds/{GID}"
    sent = json.loads(body) if body else {}

    def new_id() -> str:
        state["next"] += 1
        return str(state["next"])

    def find(rows, identifier):
        return next((row for row in rows if row["id"] == identifier), None)

    def webhooks_changed(channel: str) -> None:
        broadcast(0, {"guild_id": str(GID), "channel_id": channel}, "WEBHOOKS_UPDATE")

    def app_permissions(app: str):
        """Every overwrite entry of the app: its own and its commands'."""
        commands = [row["id"] for row in state["commands"] if row["application_id"] == app]
        return [{"id": target, "application_id": app, "guild_id": str(GID), "permissions": entries}
                for target, entries in state["permissions"].items() if (target == app or target in commands) and entries]
    if command == "GET" and path == guild + "/integrations":
        state["integration_lists"] += 1
        log_line(f"automation integrations listed {state['integration_lists']}")
        automation_external("integrations")
        return 200, state["integrations"]
    match = re.fullmatch(re.escape(guild) + r"/integrations/(\d+)", path)
    if command == "DELETE" and match and find(state["integrations"], match.group(1)):
        removed = find(state["integrations"], match.group(1))
        state["integrations"] = [row for row in state["integrations"] if row["id"] != match.group(1)]
        broadcast(0, {"id": removed["id"], "guild_id": str(GID), "application_id": removed["application"]["id"]},
                  "INTEGRATION_DELETE")
        broadcast(0, {"guild_id": str(GID)}, "GUILD_INTEGRATIONS_UPDATE")
        return 204, None
    if command == "GET" and path == guild + "/webhooks":
        if find(state["webhooks"], "660000000000000003"):
            log_line("automation webhooks listed with the external one")
        automation_external("webhooks")
        return 200, state["webhooks"]
    match = re.fullmatch(r"/channels/(\d+)/webhooks", path)
    if command == "POST" and match:
        if not 1 <= len(sent.get("name") or "") <= 80:
            return 400, {"message": "Invalid Form Body", "code": 50035}
        hook = {"id": new_id(), "type": 1, "guild_id": str(GID), "channel_id": match.group(1), "name": sent["name"],
                "avatar": None, "application_id": None, "token": "qa-new-token",
                "user": {"id": str(UID), "username": "qatester", "global_name": None}}
        state["webhooks"].append(hook)
        webhooks_changed(match.group(1))
        return 200, hook
    match = re.fullmatch(r"/webhooks/(\d+)", path)
    hook = find(state["webhooks"], match.group(1)) if match else None
    if hook and command == "PATCH":
        hook["name"] = sent.get("name", hook["name"])
        hook["channel_id"] = sent.get("channel_id", hook["channel_id"])
        if "avatar" in sent:
            hook["avatar"] = "5ec0de" if sent["avatar"] else None
        webhooks_changed(hook["channel_id"])
        return 200, {key: value for key, value in hook.items() if key != "token"}
    if hook and command == "DELETE":
        state["webhooks"].remove(hook)
        webhooks_changed(hook["channel_id"])
        return 204, None
    match = re.fullmatch(re.escape(guild) + r"/application-commands/(\d+)", path)
    if match and command == "GET":
        app = match.group(1)
        if state["external"] and str(state["commands"][0]["id"]) in state["permissions"]:
            log_line("automation commands listed with the external permissions")
        return 200, {"application_commands": [row for row in state["commands"] if row["application_id"] == app],
                     "permissions": app_permissions(app)}
    match = re.fullmatch(r"/applications/(\d+)/guilds/" + str(GID) + r"/commands(?:/(\d+))?/permissions", path)
    if match and command == "GET" and not match.group(2):
        app = match.group(1)
        if str(state["commands"][0]["id"]) in state["permissions"] and "permissions" in state["external"]:
            log_line("automation command permissions listed with the external change")
        automation_external("permissions")
        return 200, app_permissions(app)
    if match and command == "PUT" and match.group(2) and len(sent.get("permissions", [])) <= 100:
        state["permissions"][match.group(2)] = sent["permissions"]
        reply = {"id": match.group(2), "application_id": match.group(1), "guild_id": str(GID),
                 "permissions": sent["permissions"]}
        broadcast(0, reply, "APPLICATION_COMMAND_PERMISSIONS_UPDATE")
        return 200, reply
    if path == guild + "/auto-moderation/rules":
        if command == "GET":
            if find(state["rules"], "670000000000000009"):
                log_line("automation rules listed with the external one")
            automation_external("rules")
            return 200, state["rules"]
        if command == "POST":
            trigger = sent.get("trigger_type")
            same = sum(1 for rule in state["rules"] if rule["trigger_type"] == trigger)
            if same >= (6 if trigger == 1 else 1):
                return 400, {"message": "Maximum number of rules of this trigger type reached", "code": 30037}
            rule = dict(sent, id=new_id(), guild_id=str(GID), creator_id=str(UID))
            state["rules"].append(rule)
            broadcast(0, rule, "AUTO_MODERATION_RULE_CREATE")
            return 200, rule
    match = re.fullmatch(re.escape(guild) + r"/auto-moderation/rules/(\d+)", path)
    rule = find(state["rules"], match.group(1)) if match else None
    if rule and command == "PATCH":
        rule.update({key: value for key, value in sent.items() if key not in ("id", "guild_id", "creator_id")})
        broadcast(0, rule, "AUTO_MODERATION_RULE_UPDATE")
        return 200, rule
    if rule and command == "DELETE":
        state["rules"].remove(rule)
        broadcast(0, rule, "AUTO_MODERATION_RULE_DELETE")
        return 204, None
    return None


# Server Settings' Access, Enable Community / Community Overview and Server Template pages: the guild fields they
# change (merged over guild_object), the membership form, the template and Server Discovery (Userdoccers: Guilds >
# Member Verification, Guild Templates, Discovery). A guild PATCH and a form PATCH push GUILD_UPDATE.
GUILD_OVERRIDES = {}
EVERYONE_PERMISSIONS = {"value": 1071698660929}
ACCESS = {
    "form": None,  # no form until one is saved: GET answers 404 (Unknown Guild Member Verification Form)
    "template": None,
    "requirements": {"guild_id": str(GID), "safe_environment": True, "healthy": True, "health_score_pending": False,
                     "size": True, "nsfw_properties": {}, "protected": True, "sufficient": True,
                     "sufficient_without_grace_period": True, "valid_rules_channel": True, "retention_healthy": True,
                     "engagement_healthy": True, "age": True, "minimum_age": 56, "health_score": None,
                     "minimum_size": 1},
    "metadata": {"guild_id": str(GID), "primary_category_id": 0, "keywords": ["qa"], "emoji_discoverability_enabled": True,
                 "partner_actioned_timestamp": None, "partner_application_timestamp": None, "is_published": False,
                 "reasons_to_join": [], "social_links": None, "about": None, "category_ids": []},
    "categories": [{"id": 0, "name": "General", "is_primary": False}, {"id": 1, "name": "Gaming", "is_primary": True},
                   {"id": 3, "name": "Music", "is_primary": True}, {"id": 48, "name": "Esports", "is_primary": False}],
    "pending_requests": 2,  # applications waiting while Apply to Join is on
}


def guild_state() -> dict:
    guild = guild_object()
    guild.update(GUILD_OVERRIDES)
    guild["roles"][0]["permissions"] = EVERYONE_PERMISSIONS["value"]
    return guild


def guild_updated() -> dict:
    guild = guild_state()
    for push in list(GATEWAYS):
        push(0, {key: value for key, value in guild.items() if key not in ("channels", "threads", "members",
                 "presences", "voice_states", "stage_instances", "guild_scheduled_events")}, "GUILD_UPDATE")
    return guild


def set_features(names) -> None:
    GUILD_FEATURES[:] = [name for name in dict.fromkeys(names)]


def access_api(command: str, path: str, body: bytes):
    """(status, value) for the Access, Community and Server Template pages' routes, or None for others."""
    state, guild = ACCESS, f"/guilds/{GID}"
    sent = json.loads(body) if body else {}
    now = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
    if path == guild and command == "PATCH":
        for key in ("description", "verification_level", "explicit_content_filter", "default_message_notifications",
                    "preferred_locale", "owner_configured_content_level", "safety_alerts_channel_id"):
            if key in sent:
                GUILD_OVERRIDES[key] = sent[key]
        if "discovery_splash" in sent:  # a data URI becomes a made-up hash
            GUILD_OVERRIDES["discovery_splash"] = "d15c0" + "0" * 27 if sent["discovery_splash"] else None
        for key in ("rules_channel_id", "public_updates_channel_id"):
            if key in sent:  # "1" asks for a new channel: the fake answers with #general
                GUILD_OVERRIDES[key] = CID if sent[key] == "1" else (int(sent[key]) if sent[key] else None)
        if "features" in sent:
            if "COMMUNITY" in sent["features"] and "COMMUNITY" not in GUILD_FEATURES and \
                    not sent.get("public_updates_channel_id") and not GUILD_OVERRIDES.get("public_updates_channel_id"):
                return 400, {"message": "Invalid Form Body", "code": 50035}
            set_features(sent["features"])
        return 200, quoted(guild_updated())
    if path == f"{guild}/roles/{GID}" and command == "PATCH":
        if "permissions" in sent:
            EVERYONE_PERMISSIONS["value"] = int(sent["permissions"])
        role = guild_state()["roles"][0]
        for push in list(GATEWAYS):
            push(0, {"guild_id": GID, "role": role}, "GUILD_ROLE_UPDATE")
        return 200, quoted(role)
    if EXTRA_ROLE and path == f"{guild}/roles/{EXTRA_ROLE['id']}" and command == "PATCH":
        for key in ("name", "color", "hoist", "mentionable", "permissions"):
            if key in sent:
                EXTRA_ROLE[key] = int(sent[key]) if key == "permissions" else sent[key]
        for push in list(GATEWAYS):
            push(0, {"guild_id": GID, "role": quoted(dict(EXTRA_ROLE))}, "GUILD_ROLE_UPDATE")
        return 200, quoted(dict(EXTRA_ROLE))
    if path == f"{guild}/member-verification" and command == "GET":
        if state["form"] is None:
            return 404, {"message": "Unknown Guild Member Verification Form", "code": 10068}
        return 200, state["form"]
    if path == f"{guild}/member-verification" and command == "PATCH":
        fields = sent.get("form_fields", (state["form"] or {}).get("form_fields", []))
        state["form"] = {"version": now, "form_fields": fields, "description": None}
        features = [name for name in GUILD_FEATURES if name not in ("MEMBER_VERIFICATION_GATE_ENABLED",
                                                                   "MEMBER_VERIFICATION_MANUAL_APPROVAL")]
        if sent.get("enabled"):
            features.append("MEMBER_VERIFICATION_GATE_ENABLED")
            if any(field["field_type"] != "TERMS" for field in fields):
                features.append("MEMBER_VERIFICATION_MANUAL_APPROVAL")
        set_features(features)
        if sent.get("bulk_action"):  # the waiting applications are approved or rejected at once
            state["pending_requests"] = 0
        guild_updated()
        return 200, state["form"]
    if path == f"{guild}/requests" and command == "GET":  # the waiting applications (status=SUBMITTED)
        requests = [{"id": str(800000000000000001 + i), "user_id": str(UID + 1), "guild_id": str(GID),
                     "application_status": "SUBMITTED", "created_at": now, "form_responses": []}
                    for i in range(state["pending_requests"])]
        return 200, {"guild_join_requests": requests, "total": len(requests), "limit": 25}
    if path == f"{guild}/templates" and command == "GET":
        return 200, [state["template"]] if state["template"] else []
    if path == f"{guild}/templates" and command == "POST":
        if state["template"]:
            return 400, {"message": "A server can only have one template", "code": 30031}
        state["template"] = {"code": "QaTemplate42", "name": sent.get("name"), "description": sent.get("description"),
                             "usage_count": 0, "creator_id": str(UID), "creator": quoted(user_object(UID)),
                             "created_at": now, "updated_at": now, "source_guild_id": str(GID),
                             "serialized_source_guild": {"name": "QA Guild", "roles": [
                                 {"id": index, "name": role["name"]} for index, role in enumerate(guild_state()["roles"])],
                                 "channels": [{"id": c["id"], "type": c["type"], "name": c["name"],
                                               "parent_id": c["parent_id"]} for c in guild_state()["channels"]]},
                             "is_dirty": True}
        return 200, state["template"]
    match = re.fullmatch(re.escape(guild) + r"/templates/(\w+)", path)
    if match:
        template = state["template"]
        if not template or template["code"] != match.group(1):
            return 404, {"message": "Unknown Guild Template", "code": 10057}
        if command == "PUT":
            template.update(updated_at=now, is_dirty=None)
        elif command == "PATCH":
            template.update({key: sent[key] for key in ("name", "description") if key in sent}, updated_at=now)
        elif command == "DELETE":
            state["template"] = None
        return 200, template
    if path == f"{guild}/discovery-requirements" and command == "GET":
        return 200, state["requirements"]
    if path == f"{guild}/discovery-metadata" and command in ("GET", "PATCH"):
        if command == "PATCH":
            state["metadata"].update({key: value for key, value in sent.items() if key != "guild_id"})
        return 200, state["metadata"]
    if path == "/discovery/categories" and command == "GET":
        return 200, state["categories"]
    match = re.fullmatch(re.escape(guild) + r"/discovery-categories/(\d+)", path)
    if match and command in ("PUT", "DELETE"):
        ids = [i for i in state["metadata"]["category_ids"] if i != int(match.group(1))]
        state["metadata"]["category_ids"] = ids + [int(match.group(1))] if command == "PUT" else ids
        return 204, None
    return None


# The account's own settings (My Account, Devices, Connected Apps, Data & Privacy): the probe signs in with password
# "hunter2"; an authenticator code is "123456", an emailed or texted code "424242". MFA, once on, makes the sensitive
# routes ask for verification (401, code 60003) until /mfa/finish hands out the token sent back in
# X-Discord-MFA-Authorization.
ACCOUNT = {"username": "qa", "email": "qa@example.invalid", "phone": None, "mfa": False, "sms": False,
           "usage_statistics": True, "harvest": None,
           "keys": [{"id": "1219430671865610261", "type": 1, "name": "QA Key", "last_used": None}],
           "sessions": [{"id_hash": "cur", "approx_last_used_time": "2026-11-08T12:00:00+00:00",
                         "client_info": {"os": "Linux", "platform": "Discord Client", "location": "Copenhagen, Denmark"}},
                        {"id_hash": "phone1", "approx_last_used_time": "2026-11-01T12:00:00+00:00",
                         "client_info": {"os": "Android", "platform": "Discord Android", "location": "Aarhus, Denmark"}}],
           "tokens": [{"id": "11", "scopes": ["identify", "guilds"], "application": {"id": "5", "name": "QA Bot"}}],
           "connections": [{"type": "steam", "id": "7656", "name": "qa_steam", "verified": True, "visibility": 1,
                            "show_activity": True, "metadata_visibility": 0, "revoked": False, "friend_sync": False,
                            "two_way_link": False, "integrations": []}]}
# Registering a key: the creation challenge the fake answers (base64url of 1 2 3 4) and the ticket of its second request.
KEY_CHALLENGE = "AQIDBA"
KEY_TICKET = "QA.key.ticket"
MFA_TOKEN = "QA.mfa.verified"


def account_user() -> dict:
    state = ACCOUNT
    types = ([2] if state["mfa"] else []) + ([3] if state["sms"] else []) + ([1] if state["mfa"] and state["keys"] else [])
    return dict(user_object(UID), username=state["username"], email=state["email"], verified=True, locale="en-US",
                phone=state["phone"], mfa_enabled=state["mfa"], flags=16 if state["sms"] else 0,
                authenticator_types=types)


def account_api(command: str, path: str, body: bytes, headers):
    """(status, value) for the account settings routes, or None for others."""
    state = ACCOUNT
    sent = json.loads(body) if body else {}
    verified = headers.get("X-Discord-MFA-Authorization") == MFA_TOKEN
    password_error = (400, {"message": "Invalid Form Body", "code": 50035, "errors": {"password": {"_errors": [
        {"code": "PASSWORD_DOES_NOT_MATCH", "message": "Password does not match."}]}}})

    def challenge(password_only=False):
        methods = [{"type": "password"}] if password_only else \
            [{"type": "totp", "backup_codes_allowed": True}] + ([{"type": "sms"}] if state["sms"] else [])
        return 401, {"message": "Two factor is required for this operation", "code": 60003,
                     "mfa": {"ticket": "QA.ticket", "methods": methods}}

    def user_updated():
        me = account_user()
        for push in list(GATEWAYS):
            push(0, me, "USER_UPDATE")
        return me
    if command == "GET" and path == "/users/@me":
        return 200, account_user()
    if command == "PATCH" and path == "/users/@me" and any(key in sent for key in ("username", "email", "new_password")):
        if sent.get("password") != "hunter2":
            return password_error
        if state["mfa"] and not verified:
            return challenge()
        if "email" in sent:
            state["email"] = sent["email"]
        if "username" in sent:
            state["username"] = sent["username"]
        me = user_updated()
        return 200, dict(me, token="QA.fake.token.2") if "new_password" in sent else me
    if command == "POST" and path == "/mfa/finish":
        good = sent.get("data") == ("hunter2" if sent.get("mfa_type") == "password" else "123456")
        return (200, {"token": MFA_TOKEN}) if sent.get("ticket") == "QA.ticket" and good else \
            (400, {"message": "Invalid two-factor code", "code": 60008})
    if command == "POST" and path == "/auth/mfa/sms/send":
        return 200, {"phone": "+*******4567"}
    if command == "POST" and path == "/users/@me/mfa/totp/enable":
        if not verified:
            return challenge(password_only=True)
        if sent.get("code") != "123456" or len(sent.get("secret", "")) != 32:
            return 400, {"message": "Invalid two-factor code", "code": 60008}
        state["mfa"] = True
        user_updated()
        return 200, {"token": "QA.fake.token.3", "backup_codes": [
            {"user_id": str(UID), "code": code, "consumed": False} for code in ("qa2fbk01", "qa2fbk02")]}
    if command == "POST" and path == "/users/@me/mfa/totp/disable":
        if not verified:
            return challenge()
        state["mfa"] = state["sms"] = False
        user_updated()
        return 200, {"token": "QA.fake.token.4"}
    if command == "POST" and path in ("/users/@me/mfa/sms/enable", "/users/@me/mfa/sms/disable"):
        if path.endswith("disable") and sent.get("password") != "hunter2":
            return password_error
        if path.endswith("enable") and not verified:
            return challenge(password_only=True)
        state["sms"] = path.endswith("enable")
        user_updated()
        return 204, None
    if command == "POST" and path == "/auth/verify/view-backup-codes-challenge":
        return (200, {"nonce": "QA.nonce", "regenerate_nonce": "QA.regenerate"}) if sent.get("password") == "hunter2" \
            else password_error
    if command == "POST" and path == "/users/@me/mfa/codes-verification":
        if sent.get("key") != "424242":
            return 400, {"message": "Invalid verification key", "code": 50035}
        prefix = "qanew" if sent.get("regenerate") else "qa2fbk"
        return 200, {"backup_codes": [{"user_id": str(UID), "code": f"{prefix}{n:02}"[:8], "consumed": n == 1}
                                      for n in range(1, 4)]}
    if command == "POST" and path == "/users/@me/mfa/webauthn/credentials":
        # Registering a key: no parameters answer a ticket and the creation challenge; the second request names the key,
        # carries the ticket and the credential the key made, whose client data must be that challenge's webauthn.create.
        if state["mfa"] and not verified:
            return challenge()
        creation = {"publicKey": {
            "challenge": KEY_CHALLENGE, "rp": {"id": "discord.com", "name": "Discord"},
            "user": {"id": base64.urlsafe_b64encode(str(UID).encode()).rstrip(b"=").decode(),
                     "name": state["username"], "displayName": state["username"]},
            "pubKeyCredParams": [{"type": "public-key", "alg": -7}, {"type": "public-key", "alg": -257}],
            "timeout": 60000,
            "excludeCredentials": [{"type": "public-key", "id": "Y3JlZC1pZC0x"}],
            "authenticatorSelection": {"residentKey": "discouraged", "requireResidentKey": False,
                                       "userVerification": "preferred"},
            "attestation": "none"}}
        if not sent:
            return 200, {"ticket": KEY_TICKET, "challenge": json.dumps(creation)}
        try:
            credential = json.loads(sent.get("credential", ""))
            padded = credential["response"]["clientDataJSON"] + "=" * (-len(credential["response"]["clientDataJSON"]) % 4)
            client_data = json.loads(base64.urlsafe_b64decode(padded))
            valid = (sent.get("ticket") == KEY_TICKET and 1 <= len(sent.get("name", "")) <= 32 and
                     credential["type"] == "public-key" and credential["id"] == credential["rawId"] and
                     bool(credential["response"]["attestationObject"]) and
                     client_data == {"type": "webauthn.create", "challenge": KEY_CHALLENGE,
                                     "origin": "https://discord.com", "crossOrigin": False})
        except (ValueError, KeyError, TypeError):
            valid = False
        if not valid:
            return 400, {"message": "Invalid Form Body", "code": 50035}
        key = {"id": str(1219430671865610262 + len(state["keys"])), "type": 1, "name": sent["name"], "last_used": None}
        first = not state["mfa"]
        state["keys"].append(key)
        state["mfa"] = True
        user_updated()
        codes = [{"user_id": str(UID), "code": code, "consumed": False} for code in ("qakey001", "qakey002")]
        return 200, dict(key, backup_codes=codes) if first else key
    if command == "GET" and path == "/users/@me/mfa/webauthn/credentials":
        return 200, state["keys"]
    match = re.fullmatch(r"/users/@me/mfa/webauthn/credentials/(\d+)", path)
    if match:
        key = next((k for k in state["keys"] if k["id"] == match.group(1)), None)
        if not key:
            return 404, {"message": "Unknown authenticator", "code": 10070}
        if command == "PATCH":
            key["name"] = sent.get("name", key["name"])
            return 200, key
        if command == "DELETE":
            if state["mfa"] and not verified:
                return challenge()
            state["keys"].remove(key)
            return 204, None
    if command == "PUT" and path == "/users/@me/email":
        return 204, None
    if command == "POST" and path == "/users/@me/email/verify-code":
        return (200, {"token": "QA.email.token"}) if sent.get("code") == "424242" else \
            (400, {"message": "Invalid verification code", "code": 50035})
    if command == "POST" and path == "/users/@me/phone":
        if "phone" in sent:
            return 204, None
        if sent.get("password") != "hunter2":
            return password_error
        state["phone"] = "+15551234567"
        user_updated()
        return 204, None
    if command == "POST" and path == "/phone-verifications/verify":
        return (200, {"token": "QA.phone.token"}) if sent.get("code") == "424242" else \
            (400, {"message": "Invalid verification code", "code": 50035})
    if command == "DELETE" and path == "/users/@me/phone":
        if sent.get("password") != "hunter2":
            return password_error
        state["phone"] = None
        user_updated()
        return 204, None
    if command == "POST" and path in ("/users/@me/disable", "/users/@me/delete"):
        return (204, None) if sent.get("password") == "hunter2" else password_error
    if command == "GET" and path == "/auth/sessions":
        return 200, {"user_sessions": state["sessions"]}
    if command == "POST" and path == "/auth/sessions/logout":
        gone = set(sent.get("session_id_hashes", []))
        state["sessions"] = [s for s in state["sessions"] if s["id_hash"] not in gone]
        return 204, None
    if command == "GET" and path == "/oauth2/tokens":
        return 200, state["tokens"]
    match = re.fullmatch(r"/oauth2/tokens/(\d+)", path)
    if match and command == "DELETE":
        state["tokens"] = [t for t in state["tokens"] if t["id"] != match.group(1)]
        return 204, None
    if command == "GET" and path == "/users/@me/connections":
        return 200, state["connections"]
    match = re.fullmatch(r"/users/@me/connections/([\w-]+)/([^/]+)", path)
    if match:
        connection = next((c for c in state["connections"] if c["type"] == match.group(1) and
                           c["id"] == urllib.parse.unquote(match.group(2))), None)
        if not connection:
            return 404, {"message": "Unknown Connection", "code": 10017}
        if command == "PATCH":
            connection.update({key: sent[key] for key in ("visibility", "show_activity", "metadata_visibility")
                               if key in sent})
            return 200, connection
        if command == "DELETE":
            state["connections"].remove(connection)
            return 204, None
    if path == "/users/@me/consent" and command in ("GET", "POST"):
        if "usage_statistics" in sent.get("grant", []):
            state["usage_statistics"] = True
        if "usage_statistics" in sent.get("revoke", []):
            state["usage_statistics"] = False
        consents = {"personalization": {"consented": True}}
        return 200, dict(consents, usage_statistics={"consented": state["usage_statistics"]}) \
            if command == "GET" else consents
    if path == "/users/@me/harvest" and command == "GET":
        return (200, state["harvest"]) if state["harvest"] else (204, None)
    if path == "/users/@me/harvest" and command == "POST":
        if not sent.get("backends"):
            return 400, {"message": "Invalid Form Body", "code": 50035}
        state["harvest"] = {"harvest_id": "1", "user_id": str(UID), "email": state["email"], "state": "INCOMPLETE",
                            "status": 0, "created_at": time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime()),
                            "completed_at": None, "polled_at": None, "backends": {}, "shadow_run": False}
        return 200, state["harvest"]
    return profile_editor_api(command, path.split("?")[0], sent)


# The profile editor (Settings > Profiles): its load (GET /users/{UID}/profile), its saves (PATCH /users/@me,
# /users/@me/profile and the per-server PATCH /guilds/{GID}/members/@me) and the owned collectibles: one nameplate,
# whose still the CDN serves at media/v1/collectibles-shop/{sku}/static as the member list and the editor's preview read
# it. MAICORD_FAKE_NITRO=1 makes the account Nitro (premium type 2); without it the editor offers Try It Out.
NITRO = bool(os.environ.get("MAICORD_FAKE_NITRO"))
NAMEPLATE_SKU = 1344802364934062152
NAMEPLATE = {"sku_id": NAMEPLATE_SKU, "asset": "nameplates/n/angel/", "label": "Angel", "palette": "bubble_gum"}
FILES[f"/media/v1/collectibles-shop/{NAMEPLATE_SKU}/static"] = (
    "image/png", png(288, 64, lambda x, y: (120 + x * 120 // 288, 60, 200 - x * 120 // 288)))
EDITOR = {"bio": "", "pronouns": "", "banner": None, "accent_color": None, "theme_colors": None,
          "display_name_styles": None, "global_name": "QA Tester", "nameplate": None}
# The profile's connected accounts (Userdoccers: Connected Accounts, partial connection): Steam with its metadata, GitHub
# and Spotify, as the full profile and the DM panel list them. Their logos are files of the web client's asset host
# (discord.com/assets/<hash>.png, flat squares here), as are the two game pictures beside Steam's item counts.
CONNECTED_ACCOUNTS = [
    {"type": "steam", "id": "76561198000000001", "name": "Zupko", "verified": True,
     "metadata": {"created_at": "2020-01-19T12:00:00+00:00", "game_count": "172", "item_count_dota2": "0",
                  "item_count_tf2": "84"}},
    {"type": "github", "id": "583231", "name": "Zupk", "verified": True, "metadata": {}},
    {"type": "spotify", "id": "zupk", "name": "Zupk Listener", "verified": True},
]
for _hash, _tint in (("b68f2f4df4af9085", (23, 26, 33)), ("1cc92a6d139af44d", (36, 41, 47)),
                     ("40518c6ced9fed93", (30, 215, 96)), ("44e2229fd99bc14e", (190, 40, 30)),
                     ("9eb53b4fa3cc12d5", (207, 106, 50))):
    FILES[f"/assets/{_hash}.png"] = ("image/png", png(32, 32, lambda x, y, t=_tint: t))


def profile_editor_api(command: str, route: str, sent: dict):
    """(status, value) for the profile editor's routes, or None for others."""
    state = EDITOR
    if command == "GET" and route == f"/users/{UID}/profile":
        user = dict(account_user(), global_name=state["global_name"], banner=state["banner"],
                    display_name_styles=state["display_name_styles"], premium_type=2 if NITRO else 0,
                    collectibles={"nameplate": state["nameplate"]} if state["nameplate"] else None)
        profile = {key: state[key] for key in ("bio", "pronouns", "banner", "accent_color", "theme_colors")}
        return 200, {"user": user, "user_profile": dict(profile, collectibles=[]), "guild_member_profile": {},
                     "premium_type": 2 if NITRO else 0, "badges": [], "connected_accounts": CONNECTED_ACCOUNTS}
    if command == "GET" and route == "/users/@me/collectibles-purchases":
        return 200, [{"sku_id": NAMEPLATE_SKU, "name": "Angel", "items": [dict(NAMEPLATE, type=2)]}]
    if command == "PATCH" and route == "/users/@me/profile":
        for key in ("bio", "pronouns", "banner", "accent_color", "theme_colors"):
            if key in sent:
                state[key] = sent[key]
        return 200, {key: state[key] for key in ("bio", "pronouns", "banner", "accent_color", "theme_colors")}
    if command == "PATCH" and route == "/users/@me":
        if "global_name" in sent:
            state["global_name"] = sent["global_name"]
        if "nameplate_sku_id" in sent:
            state["nameplate"] = dict(NAMEPLATE) if sent["nameplate_sku_id"] else None
        if "display_name_font_id" in sent:
            state["display_name_styles"] = None if sent["display_name_font_id"] is None else {
                "font_id": sent["display_name_font_id"], "effect_id": sent["display_name_effect_id"],
                "colors": sent["display_name_colors"]}
        return 200, dict(account_user(), global_name=state["global_name"],
                         display_name_styles=state["display_name_styles"])
    if command == "PATCH" and route == f"/guilds/{GID}/members/@me":
        member = state.setdefault("member", {"nick": None, "display_name_styles": None})
        if "nick" in sent:
            member["nick"] = sent["nick"]
        if "display_name_font_id" in sent:
            member["display_name_styles"] = None if sent["display_name_font_id"] is None else {
                "font_id": sent["display_name_font_id"], "effect_id": sent["display_name_effect_id"],
                "colors": sent["display_name_colors"]}
        return 200, dict(member, user=account_user(), roles=[], joined_at="2026-01-01T00:00:00+00:00")
    return None


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    options = None
    lock = threading.Lock()

    def log_message(self, format, *arguments):
        pass

    def note(self, status):
        if self.options.log:
            with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
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

    def detectable(self, body):
        """A detectable list (the CDN's games-v1.json, GET /games/detectable/exclusions) with its ETag; a matching
        If-None-Match answers 304, as Discord's servers do."""
        tag = 'W/"' + hashlib.sha1(body).hexdigest()[:32] + '"'
        if self.headers.get("If-None-Match") == tag:
            self.note(304)
            self.send_response(304)
            self.send_header("ETag", tag)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if "gzip" in (self.headers.get("Accept-Encoding") or ""):  # as the CDN answers the client
            return self.reply(200, gzip.compress(body), extra={"ETag": tag, "Content-Encoding": "gzip"})
        self.reply(200, body, extra={"ETag": tag})

    def json(self, status, value):
        self.reply(status, json.dumps(quoted(value)).encode())

    # -- the API ------------------------------------------------------------------------------------------------------
    def api(self):
        path = self.path.split("?", 1)[0]
        path = re.sub(r"^/api/v\d+", "", path)
        if self.command == "GET" and path == "/games/detectable/exclusions":
            return self.detectable(DETECTABLE_EXCLUSIONS)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if WORLD:
            query = dict(part.split("=", 1) for part in (self.path.split("?", 1)[1:] or [""])[0].split("&") if "=" in part)
            answer = WORLD.api(self.command, path, query, body, broadcast)
            if answer:
                status, value = answer
                return self.reply(status, b"") if value is None else self.reply(status, json.dumps(value).encode())
        if self.command == "POST" and path == "/auth/login":
            return self.json(200, {"user_id": str(WORLD.me["id"] if WORLD else UID), "token": "QA.fake.token",
                                   "user_settings": {"locale": "en-US", "theme": "dark"}})
        account = account_api(self.command, path, body, self.headers)
        if account:
            if self.command != "GET" and self.options.log:
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                    handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
            status, value = account
            return self.reply(status, b"") if value is None else self.json(status, value)
        if self.command == "GET" and path == "/gateway":
            return self.json(200, {"url": "wss://gateway.discord.gg"})
        match = re.fullmatch(r"/channels/(\d+)/messages", path)
        if self.command == "GET" and match:
            query = dict(part.split("=", 1) for part in (self.path.split("?", 1)[1:] or [""])[0].split("&") if "=" in part)
            rows = message_rows(int(match.group(1))) if int(match.group(1)) in MESSAGES else []
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
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
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
        if path in (f"/guilds/{GID}/profile", "/users/@me/clan") and self.command in ("PATCH", "PUT") and self.options.log:
            with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
        games = game_route(self.command, path, (self.path.split("?", 1)[1:] or [""])[0])
        if games is not None:
            return self.json(200, games)
        if path == f"/guilds/{GID}/profile" and self.command == "GET":
            return self.json(200, PROFILE)
        if path == f"/guilds/{GID}/profile" and self.command == "PATCH":
            sent = json.loads(body or b"{}")
            tag = sent.get("tag")
            if any(key in sent for key in ("tag", "badge", "badge_color_primary", "badge_color_secondary")) and \
                    "GUILD_TAGS" not in GUILD_FEATURES:
                return self.json(403, {"message": "Missing Access", "code": 50001})
            if tag is not None and not (2 <= len(tag) <= 4 and tag.isalnum()):
                return self.json(400, {"message": "Invalid Form Body", "code": 50035, "errors": {"tag": {"_errors": [
                    {"code": "BASE_TYPE_BAD_LENGTH", "message": "Must be between 2 and 4 in length."}]}}})
            return self.json(200, profile_patch(sent))
        if path == "/users/@me/clan" and self.command == "PUT":
            sent = json.loads(body or b"{}")
            if sent.get("identity_enabled") and int(sent.get("identity_guild_id") or 0) == GID and PROFILE["tag"]:
                PRIMARY_GUILD["value"] = {"identity_guild_id": GID, "identity_enabled": True, "tag": PROFILE["tag"],
                                          "badge": PROFILE["badge_hash"]}
            elif not sent.get("identity_enabled"):
                PRIMARY_GUILD["value"] = {"identity_guild_id": None, "identity_enabled": False, "tag": None, "badge": None}
            else:
                return self.json(400, {"message": "Invalid Form Body", "code": 50035})
            me = dict(user_object(UID), email="qa@example.invalid", verified=True, locale="en-US")
            for push in list(GATEWAYS):
                push(0, me, "USER_UPDATE")
            return self.json(200, me)
        if path.startswith("/users/@me/relationships") and self.command in ("POST", "PUT", "PATCH", "DELETE"):
            return self.relationship(path, body)
        if path == f"/channels/{REQUEST_CHANNEL}/recipients/@me" and self.command in ("PUT", "DELETE"):
            if self.options.log:
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                    handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
            accepted = self.command == "PUT"
            for push in list(GATEWAYS):
                if accepted:
                    push(0, {"id": REQUEST_CHANNEL, "type": 1, "recipients": [user_object(STRANGER)],
                             "is_message_request": False, "is_spam": False}, "CHANNEL_UPDATE")
                else:
                    push(0, {"id": REQUEST_CHANNEL, "type": 1}, "CHANNEL_DELETE")
            return self.reply(204, b"")
        # User settings (settings-proto/1): a GET answers READY's settings, a PATCH what it carried; its body is logged.
        if path == "/users/@me/settings-proto/1" and self.command == "GET":
            return self.json(200, {"settings": "mAYA"})
        if path == "/users/@me/settings-proto/1" and self.command == "PATCH":
            if self.options.log:
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                    handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
            return self.json(200, {"settings": json.loads(body or b"{}").get("settings", "")})
        # Application Test Mode: the team's application 1300000000000000001 (Starfall Tactics Dev) answers the
        # access check; any other application is refused as Discord refuses one the account cannot test.
        test_mode = re.fullmatch(r"/activities/(\d+)/test-mode", path)
        if self.command == "GET" and test_mode:
            if test_mode.group(1) == "1300000000000000001":
                return self.reply(204, b"")
            return self.json(403, {"message": "Missing Access", "code": 50001})
        if self.command == "GET" and path == "/applications/public":
            query = (self.path.split("?", 1)[1:] or [""])[0]
            ids = re.findall(r"application_ids=(\d+)", query)
            return self.json(200, [{"id": i, "name": "Starfall Tactics Dev", "icon": None, "description": "",
                                    "flags": 0} for i in ids if i == "1300000000000000001"])
        answer = automation_api(self.command, path, body) or access_api(self.command, path, body)
        if answer:
            if self.command != "GET" and self.options.log:
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                    handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
            status, value = answer
            return self.reply(status, b"") if value is None else self.reply(status, json.dumps(value).encode())
        self.json(404, {"message": "404: Not Found", "code": 0})

    # -- relationships: Add Friend (POST by username) and a friend's nickname (PATCH) -------------------------------
    def relationship(self, path, body):
        if self.options.log:
            with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                handle.write(f"body {self.command} {path} {body.decode('utf-8', 'replace')}\n")
        sent = json.loads(body or b"{}")
        if self.command == "POST" and path == "/users/@me/relationships":
            # "closed.account" refuses requests (80000); any other name but the fake's other user is unknown (80004).
            name = sent.get("username")
            if name == "closed.account":
                return self.json(400, {"message": "Incoming friend requests disabled.", "code": 80000})
            if name != user_object(OTHER)["username"]:
                return self.json(400, {"message": "Unknown User", "code": 80004})
            if GAME_FRIEND:  # the other user is already a friend
                return self.json(400, {"message": "Already friends", "code": 80007})
            for push in list(GATEWAYS):
                push(0, {"id": OTHER, "type": 4, "nickname": None, "user": user_object(OTHER)}, "RELATIONSHIP_ADD")
            return self.reply(204, b"")
        match = re.fullmatch(r"/users/@me/relationships/(\d+)", path)
        if match and self.command == "PATCH":
            for push in list(GATEWAYS):
                push(0, {"id": int(match.group(1)), "type": 1, "nickname": sent.get("nickname"),
                         "user": user_object(int(match.group(1)))}, "RELATIONSHIP_UPDATE")
            return self.reply(204, b"")
        return self.reply(204, b"")

    def handle_any(self):
        host = (self.headers.get("Host") or "").split(":")[0]
        if host == self.options.catalog_host:
            return self.serve_catalog()
        if self.headers.get("Upgrade", "").lower() == "websocket":
            return self.gateway()
        if WORLD and (host == "www.youtube.com" or host == fake_discord_world.VIDEO_HOST):
            length = int(self.headers.get("Content-Length") or 0)
            answer = WORLD.site(host, self.command, self.path, self.headers, self.rfile.read(length) if length else b"")
            if not answer:
                return self.reply(404, b"", "text/plain")
            status, kind, data, extra = answer
            return self.reply(status, data, kind, extra)
        if host.startswith("cdn.") and self.path == "/detectables/games-v1.json":
            return self.detectable(DETECTABLE_GAMES)
        # The web client's assets (connection logos): GET discord.com/assets/<hash>.png.
        if host.startswith("cdn.") or host.startswith("media.") or (host == "discord.com" and self.path.startswith("/assets/")):
            found = (WORLD and WORLD.file(self.path)) or FILES.get(self.path.split("?", 1)[0])
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
        if host == STREAM_HOST:
            return stream_gateway(self.connection, self.rfile)
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
                with self.lock, open(self.options.log, "a", encoding="utf-8") as handle:
                    handle.write(f"ws op {op} at {time.time() - START:.1f}s (ws opcode {opcode})\n")
            if WORLD and WORLD.gateway(op, message.get("d"), dispatch):
                continue
            if op in (1, 40):  # the user gateway's heartbeat is op 40
                dispatch(11)
            elif op == 6:
                dispatch(0, {"_trace": ["[\"qa\",{\"micros\":0}]"]}, "RESUMED")
            elif op == 4:  # a voice state update: join (or leave) the voice channel
                voice_state(dispatch, message.get("d") or {})
            elif op in (18, 19, 20, 22):  # Go Live: create, delete, watch, pause
                stream_command(dispatch, op, message.get("d") or {})
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
    global VOICE_UDP_PORT, STREAM_UDP_PORT
    voice_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    voice_socket.bind(("127.0.0.1", 0))
    VOICE_UDP_PORT = voice_socket.getsockname()[1]
    threading.Thread(target=voice_udp, args=(voice_socket,), daemon=True).start()
    stream_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stream_socket.bind(("127.0.0.1", 0))
    STREAM_UDP_PORT = stream_socket.getsockname()[1]
    threading.Thread(target=stream_udp, args=(stream_socket,), daemon=True).start()
    print("ready", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())

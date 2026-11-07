"""A fuller world for tools/fake_discord_server.py: several guilds, people with avatars, banners and profiles, roles,
presences, a member list, voice channel occupants, direct messages and richer conversations.

MAICORD_FAKE_WORLD=DIR names a directory holding world.json and cdn/ (the files the CDN names serve, by path:
avatars/<user>/<hash>.png, icons/<guild>/<hash>.webp, banners/..., emojis/<id>.webp, pictures/<name>). Everything in it is
made up; the server answers from it instead of its built-in QA guild. world.json:

    me                  key of the signed-in account in users
    users               key -> {id, username, global_name, avatar, banner, accent_color, bio, pronouns, theme_colors, bot,
                          display_name_styles: {font_id, effect_id, colors}, primary_guild: {identity_guild_id,
                          identity_enabled, tag, badge} (the badge under cdn/clan-badges/<guild>/<badge>.png),
                          webhook (a webhook author: bot, discriminator 0000, its messages carry webhook_id, and like
                          any ID that is not a user its profile answers 404 Unknown User), profile_status (403 or 404:
                          the profile GET fails that way)}
    guilds              [{id, name, icon, description, owner, roles: [{id, name, color, hoist, position}],
                          channels: [{id, type, name, parent, topic}], members: [{user, roles, nick}],
                          emojis: [{id, name}], voice: [{user, channel, self_mute, self_video}],
                          member_count (what a large server reports; default: the members listed)}]
    presences           user key -> {status, activities: [{type, name, details, state, emoji, started_ago (ms)}]}
    dms                 [{id, type (1 or 3), users: [keys], name}]
    friends             [user keys]
    emoji               name -> id of the custom emoji ("{e:name}" in a message, a reaction by name)
    pictures            key -> {file, name, width, height} under cdn/
    messages            channel id -> [{author, content, ago (minutes), reactions: [[emoji, count, me]],
                          pictures: [keys], embeds, reply (index in the same list), poll, mentions: [keys]}];
                        an embed's image or thumbnail and its author's or footer's icon_url may name a picture key
                        content may hold {e:name}, {m:user} (a mention) and {t:minutes} (a time that many minutes away)
    unread              channel id -> mention count (the channel shows unread)
    events              name -> [{at (seconds), type: typing|message|reaction, ...}]: GET /__world/event/<name> on the
                        API host plays one, so a capture can show someone typing and a message arriving
    videos              11-character id -> {file under cdn/ (an MP4 with H.264 and AAC), width, height, seconds}: a
                        YouTube embed of that id plays the file when www.youtube.com and VIDEO_HOST point at the server

Message times are minutes before the server's start; MAICORD_FAKE_WORLD_NOW=MS (Unix milliseconds) pins that clock, so
captures made at different times (or on two systems) show the same timestamps.
"""
import json
import os
import re
import threading
import time

EPOCH = 1420070400000
CDN = "https://cdn.discordapp.com"
VIDEO_HOST = "rr1---sn-world.googlevideo.com"
PERMISSIONS = 1071698660929
NOW = int(os.environ.get("MAICORD_FAKE_WORLD_NOW") or time.time() * 1000)


def snowflake(ms: int, low: int) -> int:
    return ((ms - EPOCH) << 22) | low


def iso(ms: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms / 1000)) + ".000000+00:00"


def rest(value, inside_poll=False):
    """REST JSON writes ids as strings; a poll's answer ids stay numbers."""
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            if isinstance(v, int) and not isinstance(v, bool) and not inside_poll and (
                    k == "id" or k.endswith("_id") or k == "permissions"):
                out[k] = str(v)
            elif k.endswith("_ids") and isinstance(v, list):
                out[k] = [str(i) for i in v]
            else:
                out[k] = rest(v, inside_poll or k == "poll")
        return out
    if isinstance(value, list):
        return [rest(v, inside_poll) for v in value]
    return value


KINDS = {".png": "image/png", ".webp": "image/webp", ".jpg": "image/jpeg", ".gif": "image/gif"}


class World:
    def __init__(self, directory: str, log):
        self.directory = os.path.abspath(directory)
        self.log = log
        with open(os.path.join(self.directory, "world.json"), encoding="utf-8") as handle:
            self.data = json.load(handle)
        self.users = self.data["users"]
        self.by_id = {u["id"]: key for key, u in self.users.items()}
        self.me = self.users[self.data["me"]]
        self.guilds = self.data["guilds"]
        self.channel_guild = {c["id"]: g for g in self.guilds for c in g["channels"]}
        self.dms = {d["id"]: d for d in self.data.get("dms", [])}
        self.files = {}  # CDN path -> file under cdn/ (attachments and embed pictures)
        self.lock = threading.Lock()
        self.sequence = 0
        self.messages = {}
        for channel, items in self.data.get("messages", {}).items():
            channel = int(channel)
            rows = []
            for item in items:
                rows.append(self.make_message(channel, item, rows))
            self.messages[channel] = rows

    # -- objects ------------------------------------------------------------------------------------------------------
    def user(self, key_or_id) -> dict:
        key = key_or_id if isinstance(key_or_id, str) else self.by_id[key_or_id]
        u = self.users[key]
        discriminator = "0000" if u.get("webhook") else "1337" if u.get("bot") else "0"
        return {"id": u["id"], "username": u["username"], "discriminator": discriminator,
                "global_name": u["global_name"], "avatar": u["avatar"], "avatar_decoration_data": None,
                "banner": u.get("banner"), "accent_color": u.get("accent_color"),
                "bot": bool(u.get("bot") or u.get("webhook")),
                "system": False, "flags": 0, "public_flags": 0, "premium_type": 2 if u.get("theme_colors") else 0,
                "clan": u.get("primary_guild"), "primary_guild": u.get("primary_guild"),
                **({"display_name_styles": u["display_name_styles"]} if u.get("display_name_styles") else {})}

    def text(self, content: str) -> str:
        emoji = self.data.get("emoji", {})
        content = re.sub(r"\{e:(\w+)\}", lambda m: f"<:{m.group(1)}:{emoji[m.group(1)]}>", content)
        content = re.sub(r"\{m:(\w+)\}", lambda m: f"<@{self.users[m.group(1)]['id']}>", content)
        return re.sub(r"\{t:(-?\d+)\}", lambda m: str((NOW + int(m.group(1)) * 60000) // 1000), content)

    def emoji_object(self, name: str) -> dict:
        identifier = self.data.get("emoji", {}).get(name)
        return {"id": identifier, "name": name, "animated": False} if identifier else {"id": None, "name": name}

    def reaction(self, name, count, me) -> dict:
        return {"emoji": self.emoji_object(name), "count": count, "count_details": {"burst": 0, "normal": count},
                "me": me, "me_burst": False, "burst_me": False, "burst_count": 0, "burst_colors": []}

    def picture(self, channel: int, key: str) -> dict:
        p = self.data["pictures"][key]
        self.sequence += 1
        identifier = 860000000000000000 + self.sequence
        path = f"/attachments/{channel}/{identifier}/{p['name']}"
        self.files[path] = p["file"]
        url = f"{CDN}{path}?ex={(NOW // 1000 + 86400):x}&is={(NOW // 1000 - 3600):x}&hm={'cd' * 32}"
        size = os.path.getsize(os.path.join(self.directory, "cdn", p["file"]))
        return {"id": identifier, "filename": p["name"], "size": size, "url": url,
                "proxy_url": url.replace("cdn.discordapp.com", "media.discordapp.net"), "content_type": "image/png",
                "width": p["width"], "height": p["height"]}

    def make_message(self, channel: int, item: dict, earlier: list) -> dict:
        self.sequence += 1
        ms = NOW - int(item.get("ago", 0) * 60000)
        message = {"id": snowflake(ms, self.sequence), "channel_id": channel, "author": self.user(item["author"]),
                   "content": self.text(item.get("content", "")), "timestamp": iso(ms), "edited_timestamp": None,
                   "tts": False, "mention_everyone": False,
                   "mentions": [self.user(key) for key in item.get("mentions", [])], "mention_roles": [],
                   "attachments": [self.picture(channel, key) for key in item.get("pictures", [])], "embeds": [],
                   "pinned": False, "type": 0, "flags": 0, "components": []}
        if self.users[item["author"]].get("webhook"):
            message["webhook_id"] = self.users[item["author"]]["id"]
        for embed in item.get("embeds", []):
            embed = dict(embed)
            for slot in ("image", "thumbnail"):  # a picture key becomes that picture on the CDN
                if isinstance(embed.get(slot), str):
                    shown = self.picture(channel, embed[slot])
                    embed[slot] = {"url": shown["url"], "proxy_url": shown["proxy_url"], "width": shown["width"],
                                   "height": shown["height"]}
            for slot in ("author", "footer"):
                part = embed.get(slot)
                if isinstance(part, dict) and part.get("icon_url") in self.data["pictures"]:
                    shown = self.picture(channel, part["icon_url"])
                    embed[slot] = dict(part, icon_url=shown["url"], proxy_icon_url=shown["proxy_url"])
            message["embeds"].append(embed)
        if item.get("reactions"):
            message["reactions"] = [self.reaction(*r) for r in item["reactions"]]
        if "reply" in item:
            target = earlier[item["reply"]]
            guild = self.channel_guild.get(channel)
            message.update(type=19, message_reference={"type": 0, "message_id": target["id"], "channel_id": channel,
                                                       **({"guild_id": guild["id"]} if guild else {})},
                           referenced_message={k: v for k, v in target.items() if k != "referenced_message"})
        if item.get("poll"):
            poll = item["poll"]
            message["poll"] = {
                "question": {"text": poll["question"]},
                "answers": [{"answer_id": i + 1, "poll_media": {"text": text, "emoji": {"id": None, "name": emoji}}}
                            for i, (text, emoji, _) in enumerate(poll["answers"])],
                "expiry": iso(NOW + poll.get("hours_left", 24) * 3600000), "allow_multiselect": False,
                "layout_type": 1,
                "results": {"is_finalized": False, "answer_counts": [
                    {"id": i + 1, "count": count, "me_voted": poll.get("voted") == i}
                    for i, (_, _, count) in enumerate(poll["answers"])]}}
        if guild := self.channel_guild.get(channel):
            message["guild_id"] = guild["id"]
        return message

    def member(self, guild: dict, row: dict, with_presence=False) -> dict:
        member = {"user": self.user(row["user"]), "roles": row["roles"], "nick": row.get("nick"), "avatar": None,
                  "banner": None, "joined_at": iso(NOW - 86400000 * 200), "premium_since": None, "deaf": False,
                  "mute": False, "flags": 0, "pending": False, "communication_disabled_until": None}
        if with_presence:
            member["presence"] = self.presence(row["user"])
        return member

    def presence(self, key: str) -> dict:
        p = self.data.get("presences", {}).get(key, {"status": "offline", "activities": []})
        activities = []
        for a in p.get("activities", []):
            activity = {k: v for k, v in a.items() if k != "started_ago"}
            activity.setdefault("created_at", NOW - a.get("started_ago", 0))
            if "started_ago" in a:
                activity["timestamps"] = {"start": NOW - a["started_ago"]}
            activity["id"] = "custom" if a["type"] == 4 else f"{key}-{a['type']}"
            activities.append(activity)
        status = p["status"]
        return {"user": {"id": self.users[key]["id"]}, "status": status,
                "client_status": {} if status == "offline" else {"desktop": status},
                "activities": activities if status != "offline" else []}

    def last_id(self, channel: int):
        rows = self.messages.get(channel)
        return rows[-1]["id"] if rows else None

    def guild_object(self, g: dict) -> dict:
        everyone = {"id": g["id"], "name": "@everyone", "description": None, "permissions": PERMISSIONS,
                    "position": 0, "color": 0, "hoist": False, "managed": False, "mentionable": False, "icon": None,
                    "unicode_emoji": None, "flags": 0}
        roles = [everyone] + [dict(r, description=None, permissions=PERMISSIONS, managed=False, mentionable=True,
                                   icon=None, unicode_emoji=None, flags=0) for r in g["roles"]]
        channels = []
        position = {}
        for c in g["channels"]:
            parent = c.get("parent")
            position[parent] = position.get(parent, -1) + 1
            row = {"id": c["id"], "type": c["type"], "name": c["name"], "position": position[parent], "flags": 0,
                   "parent_id": parent, "permission_overwrites": [], "nsfw": False, "topic": c.get("topic"),
                   "rate_limit_per_user": 0, "last_message_id": self.last_id(c["id"])}
            if c["type"] in (2, 13):
                row.update(bitrate=64000, user_limit=0, rtc_region=None)
            channels.append(row)
        voice = [{"user_id": self.users[v["user"]]["id"], "channel_id": v["channel"], "session_id": f"world-{v['user']}",
                  "deaf": False, "mute": False, "self_deaf": False, "self_mute": bool(v.get("self_mute")),
                  "self_video": bool(v.get("self_video")), "self_stream": False, "suppress": False,
                  "request_to_speak_timestamp": None} for v in g.get("voice", [])]
        stages = [{"id": c["id"] + 1, "guild_id": g["id"], "channel_id": c["id"], "topic": c["name"], "privacy_level": 2,
                   "discoverable_disabled": False, "guild_scheduled_event_id": None}
                  for c in g["channels"] if c["type"] == 13]
        return {"id": g["id"], "name": g["name"], "icon": g["icon"], "splash": None, "banner": None,
                "description": g.get("description"), "owner_id": self.users[g["owner"]]["id"], "region": "us-east",
                "afk_channel_id": None, "afk_timeout": 300, "features": [], "verification_level": 0,
                "default_message_notifications": 1, "explicit_content_filter": 0, "mfa_level": 0,
                "system_channel_id": None, "system_channel_flags": 0, "rules_channel_id": None, "premium_tier": 0,
                "premium_subscription_count": 0, "preferred_locale": "en-US", "public_updates_channel_id": None,
                "nsfw_level": 0, "premium_progress_bar_enabled": False, "max_members": 500000,
                "member_count": g.get("member_count", len(g["members"])), "joined_at": iso(NOW - 86400000 * 120),
                "large": g.get("member_count", len(g["members"])) > 250,
                "lazy": True, "unavailable": False, "roles": roles, "channels": channels, "threads": [],
                "emojis": [{"id": e["id"], "name": e["name"], "animated": False, "available": True, "managed": False,
                            "require_colons": True, "roles": []} for e in g.get("emojis", [])],
                "stickers": [], "stage_instances": stages, "guild_scheduled_events": [], "voice_states": voice,
                "presences": [], "members": [], "application_command_counts": {}, "version": 1, "hub_type": None,
                "latest_on_boarding_question_id": None, "safety_alerts_channel_id": None, "permissions": PERMISSIONS,
                "incidents_data": None, "embedded_activities": [], "home_header": None}

    def ready(self) -> dict:
        me = dict(self.user(self.data["me"]), email="nova@example.invalid", verified=True, mfa_enabled=False,
                  locale="en-US", nsfw_allowed=True, purchased_flags=0, premium=False, phone=None,
                  bio=self.me.get("bio", ""), desktop=True, mobile=False)
        others = [self.user(key) for key in self.users if key != self.data["me"]]
        presences = []
        for g in self.guilds:
            presences.append([dict(self.presence(m["user"]), user_id=self.users[m["user"]]["id"])
                              for m in g["members"] if m["user"] != self.data["me"]])
        friends = [dict(self.presence(key), user_id=self.users[key]["id"]) for key in self.data.get("friends", [])]
        private = []
        for d in self.dms.values():
            row = {"id": d["id"], "type": d["type"], "recipient_ids": [self.users[k]["id"] for k in d["users"]],
                   "last_message_id": self.last_id(d["id"]), "flags": 0, "is_spam": False}
            if d["type"] == 3:
                row.update(name=d.get("name"), icon=None, owner_id=self.users[d["users"][0]]["id"])
            private.append(row)
        reads = []
        unread = {int(k): v for k, v in self.data.get("unread", {}).items()}
        for channel, rows in self.messages.items():
            last = rows[-1]["id"]
            if channel in unread and len(rows) > 1:
                last = rows[-2]["id"] if unread[channel] == 0 else rows[max(0, len(rows) - 1 - unread[channel])]["id"] - 1
            reads.append({"id": channel, "last_message_id": last, "mention_count": unread.get(channel, 0),
                          "last_pin_timestamp": iso(0), "flags": 0, "last_viewed": 0})
        return {"v": 9, "user": me, "users": others, "guilds": [self.guild_object(g) for g in self.guilds],
                "private_channels": private,
                "relationships": [{"id": self.users[k]["id"], "type": 1, "nickname": None, "user_id": self.users[k]["id"],
                                   "since": iso(NOW - 86400000 * 300)} for k in self.data.get("friends", [])],
                "merged_presences": {"guilds": presences, "friends": friends},
                "merged_members": [[dict({k: v for k, v in self.member(g, m).items() if k != "user"},
                                         user_id=self.users[m["user"]]["id"]) for m in g["members"]] for g in self.guilds],
                "read_state": {"version": 1, "partial": False, "entries": reads},
                "user_guild_settings": {"version": 1, "partial": False, "entries": [
                    {"guild_id": g["id"], "message_notifications": 1, "muted": False, "mute_config": None,
                     "suppress_everyone": False, "suppress_roles": False, "mobile_push": True,
                     "hide_muted_channels": False, "notify_highlights": 0, "flags": 0, "version": 1,
                     "channel_overrides": []} for g in self.guilds]},
                "session_id": "world-session-1", "session_type": "normal", "user_settings_proto": "mAYA",
                "resume_gateway_url": "wss://gateway.discord.gg", "analytics_token": "world",
                "auth_session_id_hash": "world", "country_code": "US", "guild_join_requests": [],
                "connected_accounts": [], "consents": {"personalization": {"consented": False}}, "tutorial": None,
                "notification_settings": {"flags": 0}, "geo_ordered_rtc_regions": [], "game_relationships": [],
                "api_code_version": 1, "user_settings": {"locale": "en-US", "theme": "dark"}, "required_action": None,
                "_trace": ["[\"world\",{\"micros\":0}]"]}

    def supplemental(self) -> dict:
        return {"merged_members": [[] for _ in self.guilds], "merged_presences": {"guilds": [[] for _ in self.guilds],
                "friends": []}, "lazy_private_channels": [],
                "guilds": [{"id": g["id"], "voice_states": self.guild_object(g)["voice_states"]} for g in self.guilds]}

    # -- the member list ---------------------------------------------------------------------------------------------------
    def member_list(self, g: dict, first: int, last: int) -> dict:
        hoisted = sorted((r for r in g["roles"] if r["hoist"]), key=lambda r: -r["position"])
        buckets = {r["id"]: [] for r in hoisted}
        online, offline = [], []
        for row in sorted(g["members"], key=lambda m: self.users[m["user"]]["global_name"].lower()):
            status = "online" if row["user"] == self.data["me"] else \
                self.data.get("presences", {}).get(row["user"], {}).get("status", "offline")
            if status == "offline":
                offline.append(row)
                continue
            role = next((r["id"] for r in hoisted if r["id"] in row["roles"]), None)
            (buckets[role] if role else online).append(row)
        items, groups = [], []
        for group_id, rows in [(r["id"], buckets[r["id"]]) for r in hoisted] + [("online", online), ("offline", offline)]:
            if not rows:
                continue
            groups.append({"id": group_id, "count": len(rows)})
            items.append({"group": {"id": group_id, "count": len(rows)}})
            for row in rows:
                member = self.member(g, row, True)
                if row["user"] == self.data["me"]:
                    member["presence"] = {"user": {"id": self.me["id"]}, "status": "online",
                                          "client_status": {"desktop": "online"}, "activities": []}
                items.append({"member": member})
        online_count = len(g["members"]) - len(offline)
        return {"guild_id": g["id"], "id": "everyone", "member_count": g.get("member_count", len(g["members"])),
                "online_count": online_count,
                "groups": groups, "ops": [{"op": "SYNC", "range": [first, last], "items": items[first:last + 1]}]}

    def guild(self, identifier) -> dict:
        identifier = int(identifier)
        return next((g for g in self.guilds if g["id"] == identifier), None)

    # -- the gateway ------------------------------------------------------------------------------------------------------
    def gateway(self, op: int, data, dispatch) -> bool:
        """Answers a user gateway op the world knows; False leaves it to the server."""
        if op in (8, 14, 37):
            self.log(f"world op {op} {json.dumps(data, default=str)[:600]}")
        if op == 2:
            dispatch(0, self.ready(), "READY")
            dispatch(0, self.supplemental(), "READY_SUPPLEMENTAL")
            return True
        if op in (14, 37):
            requests = {data.get("guild_id"): data} if op == 14 else (data or {}).get("subscriptions", {})
            for guild_id, subscription in requests.items():
                g = self.guild(guild_id) if guild_id is not None else None
                wanted = {int(u) for u in (subscription or {}).get("members") or []}
                if g and wanted:  # the members key: the users a view follows (message authors, a profile)
                    rows = [m for m in g["members"] if self.users[m["user"]]["id"] in wanted]
                    dispatch(0, {"guild_id": g["id"], "members": [self.member(g, m) for m in rows],
                                 "presences": [self.presence(m["user"]) for m in rows], "chunk_index": 0,
                                 "chunk_count": 1, "not_found": []}, "GUILD_MEMBERS_CHUNK")
                channels = (subscription or {}).get("channels") or {}
                if not g or not channels:
                    continue
                ranges = next(iter(channels.values())) or [[0, 99]]
                last = max(int(r[1]) for r in ranges)
                self.log(f"world member list {g['name']} 0-{last}")
                dispatch(0, self.member_list(g, 0, last), "GUILD_MEMBER_LIST_UPDATE")
            return True
        if op == 8:
            g = self.guild((data or {}).get("guild_id", 0) if not isinstance((data or {}).get("guild_id"), list)
                           else data["guild_id"][0])
            if g:
                wanted = {int(u) for u in (data.get("user_ids") or [])}
                rows = [m for m in g["members"] if not wanted or self.users[m["user"]]["id"] in wanted]
                chunk = {"guild_id": g["id"], "members": [self.member(g, m) for m in rows], "chunk_index": 0,
                         "chunk_count": 1, "not_found": []}
                if data.get("presences"):
                    chunk["presences"] = [self.presence(m["user"]) for m in rows]
                if data.get("nonce"):
                    chunk["nonce"] = data["nonce"]
                dispatch(0, chunk, "GUILD_MEMBERS_CHUNK")
            return True
        return False

    # -- REST ---------------------------------------------------------------------------------------------------------------
    def api(self, method: str, path: str, query: dict, body: bytes, broadcast):
        """(status, value) for a request the world answers, or None. value is JSON-able (REST ids as strings)."""
        if method == "GET" and path == "/users/@me":
            return 200, rest(dict(self.user(self.data["me"]), email="nova@example.invalid", verified=True,
                                  locale="en-US", bio=self.me.get("bio", "")))
        match = re.fullmatch(r"/channels/(\d+)/messages", path)
        if match and method == "GET":
            rows = self.messages.get(int(match.group(1)), [])
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
            return 200, rest(list(reversed(rows)))
        if match and method == "POST":
            sent = json.loads(body or b"{}")
            channel = int(match.group(1))
            message = self.add(channel, {"author": self.data["me"], "content": sent.get("content", ""), "ago": 0},
                               raw=True)
            if sent.get("nonce"):
                message["nonce"] = sent["nonce"]
            broadcast(0, message, "MESSAGE_CREATE")
            return 200, rest(message)
        match = re.fullmatch(r"/users/(\d+)/profile", path)
        if match and method == "GET":
            return self.profile(int(match.group(1)), int(query["guild_id"]) if query.get("guild_id") else None)
        # A note on a user: Discord answers 404 Unknown User when there is none (and for an ID that is not a user).
        match = re.fullmatch(r"/users/@me/notes/(\d+)", path)
        if match and method == "GET":
            return 404, {"message": "Unknown User", "code": 10013}
        match = re.fullmatch(r"/users/(\d+)", path)
        if match and method == "GET" and int(match.group(1)) in self.by_id:
            return 200, rest(self.user(int(match.group(1))))
        match = re.fullmatch(r"/channels/(\d+)/messages/(\d+)/reactions/([^/]+)/@me", path)
        if match and method in ("PUT", "DELETE"):
            self.react(int(match.group(1)), int(match.group(2)), match.group(3), self.data["me"], method == "PUT",
                       broadcast)
            return 204, None
        if method == "POST" and re.fullmatch(r"/channels/\d+/typing", path):
            return 204, None
        if method == "POST" and re.fullmatch(r"/channels/\d+/messages/\d+/ack", path):
            return 200, {"token": None}
        match = re.fullmatch(r"/__world/event/(\w+)", path)
        if match:
            self.play(match.group(1), broadcast)
            return 200, {"ok": True}
        return None

    def profile(self, user_id: int, guild_id):
        u = self.users.get(self.by_id.get(user_id), {})
        if user_id not in self.by_id or u.get("webhook") or u.get("profile_status") == 404:
            return 404, {"message": "Unknown User", "code": 10013}
        if u.get("profile_status") == 403:
            return 403, {"message": "Missing Access", "code": 50001}
        key = self.by_id[user_id]
        u = self.users[key]
        user = self.user(key)
        user["bio"] = u.get("bio", "")
        profile = {"bio": u.get("bio", ""), "pronouns": u.get("pronouns", ""), "banner": u.get("banner"),
                   "accent_color": u.get("accent_color"), "theme_colors": u.get("theme_colors"), "profile_effect": None,
                   "emoji": None, "popout_animation_particle_type": None}
        mutual = [g for g in self.guilds if any(m["user"] == key for m in g["members"])]
        result = {"user": user, "user_profile": profile, "badges": [], "guild_badges": [], "connected_accounts": [],
                  "premium_type": 2 if u.get("theme_colors") else 0, "premium_since": iso(NOW - 86400000 * 400)
                  if u.get("theme_colors") else None, "premium_guild_since": None, "legacy_username": None,
                  "mutual_guilds": [{"id": g["id"], "nick": None} for g in mutual if key != self.data["me"]],
                  "mutual_friends": [self.user(f) for f in self.data.get("friends", [])[:3]
                                     if f != key and key != self.data["me"]],
                  "mutual_friends_count": 0 if key == self.data["me"] else min(3, len(self.data.get("friends", [])))}
        g = self.guild(guild_id) if guild_id else None
        row = next((m for m in g["members"] if m["user"] == key), None) if g else None
        if row:
            member = self.member(g, row)
            result["guild_member"] = member
            result["guild_member_profile"] = {"guild_id": g["id"], "bio": "", "pronouns": "", "banner": None,
                                              "accent_color": None, "theme_colors": None}
        return 200, rest(result)

    # -- changes ------------------------------------------------------------------------------------------------------
    def add(self, channel: int, item: dict, raw=False) -> dict:
        with self.lock:
            rows = self.messages.setdefault(channel, [])
            message = self.make_message(channel, item, rows)
            message["id"] = max(snowflake(int(time.time() * 1000), self.sequence % 4096), rows[-1]["id"] + 1 if rows else 0)
            message["timestamp"] = iso(int(time.time() * 1000))
            rows.append(message)
            return message

    def react(self, channel: int, message_id: int, emoji: str, key: str, add: bool, broadcast) -> None:
        from urllib.parse import unquote
        name = unquote(emoji).split(":")[0]
        rows = self.messages.get(channel, [])
        message = next((m for m in rows if m["id"] == message_id), None)
        if not message:
            return
        mine = key == self.data["me"]
        reactions = message.setdefault("reactions", [])
        found = next((r for r in reactions if r["emoji"]["name"] == name), None)
        if add:
            if found:
                found["count"] += 1
                found["count_details"]["normal"] += 1
                found["me"] = found["me"] or mine
            else:
                reactions.append(self.reaction(name, 1, mine))
        elif found:
            found["count"] -= 1
            found["count_details"]["normal"] -= 1
            if mine:
                found["me"] = False
            if found["count"] <= 0:
                reactions.remove(found)
        event = {"user_id": self.users[key]["id"], "channel_id": channel, "message_id": message_id,
                 "emoji": self.emoji_object(name), "burst": False, "type": 0}
        if guild := self.channel_guild.get(channel):
            event["guild_id"] = guild["id"]
        broadcast(0, event, "MESSAGE_REACTION_ADD" if add else "MESSAGE_REACTION_REMOVE")

    def play(self, name: str, broadcast) -> None:
        steps = self.data.get("events", {}).get(name, [])
        self.log(f"world event {name}")

        def run():
            started = time.time()
            for step in steps:
                delay = started + step.get("at", 0) - time.time()
                if delay > 0:
                    time.sleep(delay)
                channel = step["channel"]
                guild = self.channel_guild.get(channel)
                if step["type"] == "typing":
                    event = {"channel_id": channel, "user_id": self.users[step["user"]]["id"],
                             "timestamp": int(time.time())}
                    if guild:
                        row = next(m for m in guild["members"] if m["user"] == step["user"])
                        event.update(guild_id=guild["id"], member=self.member(guild, row))
                    broadcast(0, event, "TYPING_START")
                elif step["type"] == "message":
                    message = self.add(channel, {"author": step["author"], "content": step["content"], "ago": 0})
                    if guild:
                        row = next(m for m in guild["members"] if m["user"] == step["author"])
                        message = dict(message, member={k: v for k, v in self.member(guild, row).items() if k != "user"})
                    broadcast(0, message, "MESSAGE_CREATE")
                elif step["type"] == "reaction":
                    rows = self.messages.get(channel, [])
                    if rows:
                        self.react(channel, rows[-1]["id"], step["emoji"], step["user"], True, broadcast)
        threading.Thread(target=run, daemon=True).start()

    # -- the CDN ----------------------------------------------------------------------------------------------------------
    def file(self, path: str):
        """(content type, bytes) for a CDN path, or None. Any requested image format answers from the file there is."""
        path = path.split("?", 1)[0]
        relative = self.files.get(path) or path.lstrip("/")
        if ".." in relative:
            return None
        root = os.path.join(self.directory, "cdn")
        target = os.path.join(root, relative)
        stem, extension = os.path.splitext(target)
        for candidate in [target] + [stem + other for other in (".webp", ".png", ".jpg") if other != extension]:
            if os.path.isfile(candidate):
                with open(candidate, "rb") as handle:
                    return KINDS.get(os.path.splitext(candidate)[1], "application/octet-stream"), handle.read()
        return None

    # -- the video site ---------------------------------------------------------------------------------------------------
    def site(self, host: str, method: str, target: str, headers, body: bytes):
        """(status, content type, bytes, extra headers) for the world's videos (world.json "videos": an 11-character id
        -> {file under cdn/, width, height}), or None. Answers what the client's YouTube player reads: the visitor id on
        the home page, the player response with one progressive MP4 (H.264 and AAC), and that file's byte ranges on
        VIDEO_HOST, so a YouTube embed of a made-up video plays its own local file."""
        videos = self.data.get("videos", {})
        path, _, query = target.partition("?")
        if host == "www.youtube.com" and method == "GET" and path == "/":
            return 200, "text/html", b'<script>ytcfg.set({"VISITOR_DATA":"CgtmYWtld29ybGQ%3D"});</script>', {}
        if host == "www.youtube.com" and method == "POST" and path == "/youtubei/v1/player":
            identifier = json.loads(body or b"{}").get("videoId", "")
            video = videos.get(identifier)
            if not video:
                return 200, "application/json", json.dumps({"playabilityStatus": {"status": "ERROR"}}).encode(), {}
            size = os.path.getsize(os.path.join(self.directory, "cdn", video["file"]))
            url = (f"https://{VIDEO_HOST}/videoplayback?expire={NOW // 1000 + 21600}&id={identifier}&itag=18"
                   "&mime=video%2Fmp4")
            answer = {"playabilityStatus": {"status": "OK", "playableInEmbed": True},
                      "videoDetails": {"videoId": identifier, "isLive": False},
                      "streamingData": {"expiresInSeconds": "21540", "formats": [{
                          "itag": 18, "url": url, "mimeType": 'video/mp4; codecs="avc1.42001E, mp4a.40.2"',
                          "contentLength": str(size), "bitrate": max(1, size * 8 // max(1, video.get("seconds", 10))),
                          "width": video["width"], "height": video["height"], "audioQuality": "AUDIO_QUALITY_LOW",
                          "audioChannels": 2}]}}
            return 200, "application/json", json.dumps(answer).encode(), {}
        if host == VIDEO_HOST and method == "GET" and path == "/videoplayback":
            identifier = dict(p.split("=", 1) for p in query.split("&") if "=" in p).get("id", "")
            if identifier not in videos:
                return 404, "text/plain", b"", {}
            with open(os.path.join(self.directory, "cdn", videos[identifier]["file"]), "rb") as handle:
                data = handle.read()
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", headers.get("Range") or "")
            if not match:
                return 200, "video/mp4", data, {"Accept-Ranges": "bytes"}
            first = int(match.group(1))
            last = min(int(match.group(2)) if match.group(2) else len(data) - 1, len(data) - 1)
            if first > last:
                return 416, "text/plain", b"", {"Content-Range": f"bytes */{len(data)}"}
            return 206, "video/mp4", data[first:last + 1], {"Content-Range": f"bytes {first}-{last}/{len(data)}",
                                                            "Accept-Ranges": "bytes"}
        return None


def load(log):
    directory = os.environ.get("MAICORD_FAKE_WORLD")
    return World(directory, log) if directory else None

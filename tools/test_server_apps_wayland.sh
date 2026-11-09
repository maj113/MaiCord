#!/usr/bin/env bash
# Server Settings > Integrations and AutoMod against the fake Discord, through tools/test_server_settings_wayland.sh.
#
#   tools/test_server_apps_wayland.sh --binary PATH [--out DIR]
#
# Runs on the fake's QA guild (one app with two commands, one incoming webhook, one followed channel, the mention spam
# rule):
#   * webhooks: the page reads the apps and webhooks (their avatars from the CDN), renames the webhook (PATCH
#     /webhooks/{w} with name and channel) and creates one (POST /channels/{c}/webhooks with a mascot name);
#   * followed channels: unfollowing deletes the follower webhook after the confirmation;
#   * an app's command permissions: they are read with its commands, and denying @everyone PUTs the app-wide overwrites;
#   * one command's permissions: @everyone denied and a member added by user ID, PUT under the command's ID;
#   * live updates: another moderator adds a webhook, changes the integrations and a command's permissions
#     (MAICORD_FAKE_AUTOMATION_EXTERNAL), and the open page reads each list again on the Gateway event;
#   * AutoMod: the rules are read, the mention spam rule is disabled (its whole rule PATCHed back) and the spam rule is
#     set up (POSTed with the official client's defaults); a rule another moderator creates is read again;
#   * a community server: AutoMod opens from Safety Setup.
# Exit status: 0 when every run holds, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-apps-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_server_settings_wayland.sh"
guild=200000000000000001
channel=300000000000000001
app=600000000000000001
apps="^200 GET /api/v9/guilds/$guild/integrations\\?include_applications=true&include_role_connections_metadata=true$"
hooks="^200 GET /api/v9/guilds/$guild/webhooks$"
permissions="^200 GET /api/v9/applications/$app/guilds/$guild/commands/permissions$"
commands="^200 GET /api/v9/guilds/$guild/application-commands/$app$"
rules="^200 GET /api/v9/guilds/$guild/auto-moderation/rules$"
run() {
    bash "$probe" --binary "$binary" "$@" || exit $?
}

# Integrations: View Webhooks, Edit the webhook, add "y" to its name, save, then New Webhook.
run --out "$out-webhooks" --page Integrations --expect "$apps" --expect "$hooks" \
    --expect "^200 GET /embed/avatars/[0-5]\\.png$" \
    --expect "^body PATCH /webhooks/660000000000000001 \\{\"name\":\"Captain Hooky\",\"channel_id\":\"$channel\"\\}$" \
    --expect "^200 PATCH /api/v9/webhooks/660000000000000001$" \
    --expect "^body POST /channels/$channel/webhooks \\{\"name\":\"(Captain Hook|Spidey Bot)\"\\}$" \
    wait:2 click:1079,222 wait:2 click:1113,420 wait:1 click:770,584 key:End type:y wait:1 shot:renamed \
    click:1073,760 wait:2 click:1085,290 wait:2 shot:created

# Channels Followed: Edit the followed channel, Unfollow, confirm.
run --out "$out-followed" --page Integrations --expect "$hooks" \
    --expect "^204 DELETE /api/v9/webhooks/660000000000000002$" \
    wait:2 click:1113,290 wait:2 click:1113,343 wait:1 click:444,656 wait:1 shot:confirm click:810,476 wait:2 shot:unfollowed

# The app's page: its command permissions and commands, then @everyone denied and saved.
run --out "$out-app" --page Integrations --expect "$permissions" --expect "$commands" \
    --expect "^body PUT /applications/$app/guilds/$guild/commands/$app/permissions \\{\"permissions\":\\[\\{\"id\":\"$guild\",\"type\":1,\"permission\":false\\}\\]\\}$" \
    wait:2 click:1105,420 wait:2 click:1119,430 wait:1 shot:denied click:1073,760 wait:2 shot:saved

# One command (/ping): @everyone denied, a member added by user ID, saved under the command's ID.
run --out "$out-command" --page Integrations --expect "$commands" \
    --expect "^body PUT /applications/$app/guilds/$guild/commands/600000000000000011/permissions \\{\"permissions\":\\[\\{\"id\":\"$guild\",\"type\":1,\"permission\":false\\},\\{\"id\":\"100000000000000002\",\"type\":2,\"permission\":false\\}\\]\\}$" \
    wait:2 click:1105,420 wait:3 scroll:640,500,10 wait:1 click:1113,546 wait:3 shot:command click:1119,410 wait:1 \
    click:1085,550 wait:1 click:640,440 wait:1 type:100000000000000002 wait:1 click:818,518 wait:2 shot:member \
    click:1073,760 wait:2 shot:saved

# Live updates: another moderator's webhook, integrations and command permission changes are read again.
MAICORD_FAKE_AUTOMATION_EXTERNAL=1 run --out "$out-live" --page Integrations \
    --expect "^automation webhooks listed with the external one$" --expect "^automation integrations listed 2$" \
    --expect "^automation command permissions listed with the external change$" \
    --expect "^automation commands listed with the external permissions$" \
    wait:2 click:1105,420 wait:8 shot:live

# AutoMod: Disable the mention spam rule, then Set Up spam and save it.
run --out "$out-automod" --page AutoMod --expect "$rules" \
    --expect "^body PATCH /guilds/$guild/auto-moderation/rules/670000000000000001 \\{\"id\":\"670000000000000001\",\"name\":\"Block Mention Spam\",\"guild_id\":\"$guild\",\"event_type\":1,\"trigger_type\":5,\"trigger_metadata\":\\{\"mention_total_limit\":20,\"mention_raid_protection_enabled\":false\\},\"actions\":\\[\\{\"type\":1,\"metadata\":\\{\\}\\}\\],\"enabled\":false," \
    --expect "^body POST /guilds/$guild/auto-moderation/rules \\{\"name\":\"Block Suspected Spam Content\",\"guild_id\":\"$guild\",\"event_type\":1,\"trigger_type\":3,\"actions\":\\[\\{\"type\":1,\"metadata\":\\{\\}\\}\\],\"enabled\":true," \
    wait:2 click:503,510 wait:2 shot:disabled click:437,618 wait:1 shot:editor click:1073,760 wait:2 shot:created

# AutoMod live: a rule another moderator creates is read again on AUTO_MODERATION_RULE_CREATE.
MAICORD_FAKE_AUTOMATION_EXTERNAL=1 run --out "$out-automod-live" --page AutoMod --expect "$rules" \
    --expect "^automation rules listed with the external one$" wait:7 shot:live

# A community server: AutoMod lives inside Safety Setup.
run --out "$out-community" --features COMMUNITY,GUILD_TAGS --page "Safety Setup" --expect "$rules" \
    wait:2 scroll:770,500,10 wait:1 shot:safety click:1113,576 wait:2 shot:automod

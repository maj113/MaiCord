#!/usr/bin/env bash
# Server Settings > Server Template, Access and Enable Community against the fake Discord, through
# tools/test_server_settings_wayland.sh.
#
#   tools/test_server_access_wayland.sh --binary PATH [--out DIR]
#
# Runs on the fake's QA guild (owned by the account; no membership form, no template, not a Community server at first):
#   * Server Template: a title is typed and Generate Template POSTs it (the sidebar's entry gains its unsynced dot); the
#     unsynced template is synced (PUT on its code), previewed and deleted after the confirmation (DELETE);
#   * Access: the 404 of the unset membership form reads as an empty form; Server Rules on, a rule typed in its dialog,
#     and the save PATCHes member-verification with the TERMS field and the gate on;
#   * Access, Apply to Join: the default question, the Preview dialog, a question typed in its dialog, and the save's
#     member-verification PATCH with both questions;
#   * Access, leaving Apply to Join with two applications waiting: the page reads them (GET requests?status=SUBMITTED)
#     and the save asks what becomes of them; rejecting sends bulk_action REJECTED;
#   * Access on a Community server: Discoverable reads the Discovery requirements, metadata and categories; a
#     description, the Gaming category and a tag typed in the tags dialog list the server (PATCH the guild with
#     DISCOVERABLE, then the Discovery metadata echoed back), and a subcategory is added at once (PUT
#     discovery-categories/{id});
#   * Enable Community: the setup's steps run to Finish Setup, which takes Mention @everyone from @everyone (PATCH
#     roles/{guild}) and PATCHes the guild with COMMUNITY, a new rules and updates channel, a Low verification
#     level, the media filter for everyone and Mentions Only; the server update then turns the page into Community
#     Overview.
# Exit status: 0 when every run holds, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-access-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_server_settings_wayland.sh"
guild=200000000000000001
run() {
    bash "$probe" --binary "$binary" "$@"
    local status=$?
    [ "$status" -eq 0 ] || exit "$status"
}

# Server Template: Template Title, Generate Template, then (scrolled) Sync, Preview, Delete and confirm.
run --out "$out-template" --page "Server Template" \
    --expect "^200 GET /api/v9/guilds/$guild/templates$" \
    --expect "^body POST /guilds/$guild/templates \\{\"name\":\"QAtemplate\",\"description\":\"\"\\}$" \
    --expect "^200 PUT /api/v9/guilds/$guild/templates/QaTemplate42$" \
    --expect "^200 DELETE /api/v9/guilds/$guild/templates/QaTemplate42$" \
    wait:2 click:770,478 wait:1 type:QAtemplate wait:1 click:478,731 wait:3 shot:created \
    scroll:770,500,10 wait:1 click:465,709 wait:2 shot:synced click:475,709 wait:1 shot:preview key:Escape wait:1 \
    click:617,709 wait:1 shot:confirm click:782,476 wait:2 shot:deleted

# Access: Server Rules on, Add a rule, type it in the dialog, save.
run --out "$out-rules" --page Access \
    --expect "^404 GET /api/v9/guilds/$guild/member-verification\\?with_guild=false$" \
    --expect "^body PATCH /guilds/$guild/member-verification \\{\"form_fields\":\\[\\{\"field_type\":\"TERMS\",\"label\":\"Read and agree to the server rules\",\"values\":\\[\"Bekind\"\\],\"required\":true\\}\\],\"enabled\":true\\}$" \
    wait:2 click:1119,450 wait:1 click:448,608 wait:2 type:Bekind key:Return wait:1 shot:rule \
    click:1073,760 wait:3 shot:saved

# Access: Apply to Join, Preview, a Short Answer question typed in its dialog, save.
run --out "$out-apply" --page Access \
    --expect "^body PATCH /guilds/$guild/member-verification \\{\"form_fields\":\\[\\{\"field_type\":\"TEXT_INPUT\",\"label\":\"Why do you want to join our server\\?\",\"required\":true\\},\\{\"field_type\":\"TEXT_INPUT\",\"label\":\"Whoareyou\",\"required\":true\\}\\],\"enabled\":true\\}$" \
    wait:2 click:414,296 wait:1 scroll:770,500,5 wait:1 click:442,630 wait:1 shot:preview key:Escape wait:1 \
    click:459,434 wait:2 type:Whoareyou key:Return wait:1 shot:question click:1073,760 wait:3 shot:saved

# Access: from Apply to Join with two applications waiting to Invite Only without rules; reject them and save.
run --out "$out-pending" --page Access \
    --features GUILD_TAGS,MEMBER_VERIFICATION_GATE_ENABLED,MEMBER_VERIFICATION_MANUAL_APPROVAL \
    --expect "^200 GET /api/v9/guilds/$guild/requests\\?status=SUBMITTED&limit=25$" \
    --expect "^body PATCH /guilds/$guild/member-verification \\{\"form_fields\":\\[\\],\"enabled\":false,\"bulk_action\":\"REJECTED\"\\}$" \
    wait:2 click:414,260 wait:1 click:1119,450 wait:1 click:1073,760 wait:2 shot:prompt \
    click:430,441 wait:1 click:820,494 wait:3 shot:saved

# Access on a Community server: Discoverable, a description, the Gaming category, a tag, save; then add the first
# subcategory offered (Music: Gaming is the primary one).
run --out "$out-discovery" --page Access --features COMMUNITY \
    --expect "^200 GET /api/v9/guilds/$guild/discovery-requirements$" \
    --expect "^200 GET /api/v9/discovery/categories\\?locale=en-US&primary_only=false$" \
    --expect "^body PATCH /guilds/$guild \\{\"description\":\"QAguild\",\"features\":\\[\"COMMUNITY\",\"DISCOVERABLE\"\\],\"preferred_locale\":\"en-US\"\\}$" \
    --expect "^body PATCH /guilds/$guild/discovery-metadata \\{\"primary_category_id\":1,\"emoji_discoverability_enabled\":true,.*\"keywords\":\\[\"qa\",\"fun\"\\]," \
    --expect "^20[04] PUT /api/v9/guilds/$guild/discovery-categories/3$" \
    wait:2 click:414,332 wait:2 click:770,695 wait:1 type:QAguild wait:1 scroll:770,500,4 wait:1 \
    click:584,478 wait:1 click:628,501 wait:1 shot:category scroll:770,500,20 wait:1 click:1113,350 wait:2 \
    type:,fun key:Return wait:1 shot:tags click:1073,760 wait:3 shot:listed scroll:770,500,20 wait:1 \
    click:584,368 wait:1 click:628,391 wait:2 shot:subcategory

# Enable Community: Enable Community, Get Started, Enable (safety), Next (basics), Enable (defaults), agree, Finish Setup.
run --out "$out-community" --page "Enable Community" \
    --expect "^body PATCH /guilds/$guild/roles/$guild \\{\"permissions\":\"1071698529857\"\\}$" \
    --expect "^body PATCH /guilds/$guild \\{\"features\":\\[\"GUILD_TAGS\",\"COMMUNITY\"\\],\"rules_channel_id\":\"1\",\"public_updates_channel_id\":\"1\",\"verification_level\":1,\"default_message_notifications\":1,\"explicit_content_filter\":2\\}$" \
    wait:2 click:478,244 wait:1 click:453,262 wait:1 click:438,436 wait:1 click:433,512 wait:1 click:438,472 wait:1 \
    click:1119,288 wait:1 click:450,372 wait:3 shot:overview

#!/usr/bin/env bash
# Runs the Linux client signed in, against a fake Discord (tools/fake_discord_server.py) and optionally a local plugin
# catalog, and drives it on headless sway.
#
#   tools/test_discord_wayland.sh --binary PATH [--site DIR] [--out DIR] [--root] [--dbus] [--bar] [--client-arg ARG]... STEP...
#
# No account is involved. The client reaches discord.com, gateway.discord.gg, the CDN names and the catalog host
# (<owner>.github.io, the official owner) at 127.0.0.1 in a private user, network and mount namespace (unshare -Urnm,
# a private /etc/hosts) where the fake server answers over TLS with a certificate the client trusts through
# SSL_CERT_FILE. Sign in with any e-mail and password (type: and key:Return steps); the account has one guild with one
# channel and a few messages. With --site (a catalog site the MaiCord-plugins scripts built and signed with a throwaway
# key) the client must be built with that key (MAICORD_PLUGIN_CATALOG_PUBLIC_KEY at the first configure) to trust it.
# STEP... are tools/client_flow.py's (wait:, click:, type:, key:, shot:, ...). Output, in --out (default ~/mc-discord-qa):
# client_flow's screenshots and logs and the server's (discord-requests.log lists every request and gateway op).
#
# --root makes the namespace with sudo (network and mount only) instead of a user namespace, and runs the client as the
# calling user: in a user namespace the directories root owns look owned by nobody, which the plugin loader (src/app/
# plugins/loader.cpp) refuses as the parent of a plugins directory, so a run that installs a plugin needs it.
#
# MAICORD_FAKE_EXTRA_MESSAGES=FILE adds messages to the fake server's seed (see seed_extra in tools/fake_discord_server.py: a
# JSON list of {"channel": "general"|"media", "author": "me"|"other", "content", "embeds"}), for a run that needs one particular
# message, such as an embed with a given description; the channel the client opens is general, so no navigation is needed.
# A message may also name its author (author_name: a user with that display name), mention users by display name
# (mention_names, with {m0}, {m1} in the content standing for their <@id>) and reply to an earlier entry (reply_to: its index).
#
# --dbus starts a private session bus with the desktop mocks (notifications, portals, secrets) and a swaybar that hosts the
# tray, and prints what the mocks received; --bar runs swaybar alone, which reserves the top 23 pixels of the output (the
# window sits below it: layout y of a click is the window's y plus 23). A screenshot repaints the output first, because
# headless sway otherwise shows the frame from before a window's last commits when the window covers the output.
# --audio starts a private PulseAudio server (a null sink; a speech-like signal from tools/qa_voice_signal.py as the microphone)
# for a voice run (tools/test_voice_wayland.sh); the fake server's voice server is us-qa.discord.media.
# Needs unshare, openssl, python3, sway, swaymsg, grim, wtype (tools/client_flow.py), and for --root a passwordless sudo.

set -uo pipefail

binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
site=""
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-discord-qa}"
prefix="/MaiCord-plugins"
catalog_host="maj113.github.io"
steps=()
client_args=()
root=""
dbus=""
bar=""
audio=""
audio_dir=""
signal_pid=""
cleanup() { # the private PulseAudio server, its microphone feed and the session bus, when they were started
    if [ -n "$signal_pid" ]; then kill "$signal_pid" 2>/dev/null; fi
    if [ -n "$audio_dir" ]; then PULSE_RUNTIME_PATH="$audio_dir" pulseaudio --kill 2>/dev/null; rm -rf "$audio_dir"; fi
    if [ -n "$dbus" ]; then dbus_test_stop; fi
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --site) site="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --client-arg) client_args+=("$2"); shift 2 ;;
        --root) root=1; shift ;;
        --dbus) dbus=1; shift ;;
        --bar) bar=1; shift ;;
        --audio) audio=1; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) steps+=("$1"); shift ;;
    esac
done
reexec_args=()
flow_args=()
for value in "${client_args[@]}"; do # a value may look like an option, so it is passed as --client-arg=VALUE
    reexec_args+=(--client-arg "$value")
    flow_args+=("--client-arg=$value")
done
[ -z "$bar" ] || reexec_args+=(--bar)
if [ -n "$bar" ] || { [ -n "$dbus" ] && [ -z "${MAICORD_DISCORD_NO_BAR:-}" ]; }; then
    flow_args+=(--bar) # swaybar reserves the top of the output and hosts the tray the client registers
fi
[ -x "$binary" ] || { echo "No client at $binary" >&2; exit 1; }
[ -z "$site" ] || [ -f "$site/index-4.json" ] || [ -f "$site/index-3.json" ] || { echo "No catalog site at '$site'" >&2; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
names=(discord.com gateway.discord.gg cdn.discordapp.com media.discordapp.net remote-auth-gateway.discord.gg us-qa.discord.media "$catalog_host")

if [ -z "${MAICORD_DISCORD_NAMESPACE:-}" ]; then
    mkdir -p "$out"
    out="$(cd "$out" && pwd -P)"
    [ -z "$site" ] || site="$(cd "$site" && pwd -P)"
    alt=""
    for name in "${names[@]}"; do alt+="DNS:$name,"; done
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj "/CN=discord.com" \
        -addext "subjectAltName=${alt%,}" -keyout "$out/discord-key.pem" -out "$out/discord-cert.pem" 2>/dev/null \
        || { echo "openssl could not make the certificate" >&2; exit 1; }
    printf '127.0.0.1 localhost %s\n' "${names[*]}" > "$out/discord-hosts"
    if [ -n "$dbus" ]; then # a private session bus with the desktop mocks (notifications, portals, secrets), as the user
        # shellcheck source=tools/dbus_test_bus.sh
        source "$here/dbus_test_bus.sh"
        dbus_test_available || { echo "--dbus needs dbus-daemon, python3-dbus and python3-gi" >&2; exit 1; }
        trap cleanup EXIT
        dbus_test_start_bus "$out/bus" session && dbus_test_start_mock "$out/bus" "$DBUS_TEST_ADDRESS" || exit 1
        export DBUS_SESSION_BUS_ADDRESS="$DBUS_TEST_ADDRESS"
        reexec_args+=(--dbus)
    fi
    if [ -n "$audio" ]; then # a private PulseAudio server: a null sink to play into and a speech-like signal as the microphone
        command -v pulseaudio >/dev/null && command -v pactl >/dev/null || { echo "--audio needs pulseaudio and pactl" >&2; exit 1; }
        audio_dir="$(mktemp -d)"
        trap cleanup EXIT
        XDG_RUNTIME_DIR="$audio_dir" PULSE_RUNTIME_PATH="$audio_dir" pulseaudio --daemonize=yes --exit-idle-time=-1 \
            --disallow-exit --fail=yes -n -L "module-native-protocol-unix auth-anonymous=1 socket=$audio_dir/native" \
            -L "module-null-sink sink_name=qa_speakers" \
            -L "module-pipe-source source_name=qa_microphone file=$audio_dir/microphone format=s16le rate=48000 channels=1" \
            || { echo "pulseaudio did not start" >&2; exit 1; }
        export PULSE_SERVER="unix:$audio_dir/native"
        pactl set-default-source qa_microphone
        pactl set-default-sink qa_speakers
        python3 -B "$here/qa_voice_signal.py" "$audio_dir/microphone" &
        signal_pid=$!
    fi
    if [ -n "$root" ]; then
        sudo -E unshare -nm env MAICORD_DISCORD_NAMESPACE=1 MAICORD_DISCORD_USER="$(id -un)" "$0" --binary "$binary" \
            ${site:+--site "$site"} --out "$out" "${reexec_args[@]}" "${steps[@]}"
    else
        unshare -Urnm env MAICORD_DISCORD_NAMESPACE=1 "$0" --binary "$binary" ${site:+--site "$site"} --out "$out" \
            "${reexec_args[@]}" "${steps[@]}"
    fi
    status=$?
    if [ -n "$dbus" ]; then # what the desktop mocks received (notifications, portal calls), one line each
        busctl --address "$DBUS_TEST_ADDRESS" call org.maicord.Mock /org/maicord/Mock org.maicord.Mock Take \
            2>&1 | sed 's/^as [0-9]* //; s/" "/"\n"/g' > "$out/desktop-calls.txt"
        echo "desktop calls (all of them in $out/desktop-calls.txt):"
        cut -c1-300 "$out/desktop-calls.txt" | head -40
    fi
    exit $status
fi

# Inside the namespace: loopback, the private hosts file, the server.
ip link set lo up || { echo "no loopback in the namespace" >&2; exit 1; }
mount --bind "$out/discord-hosts" /etc/hosts || { echo "could not replace /etc/hosts" >&2; exit 1; }
: > "$out/discord-requests.log"
python3 -B "$here/fake_discord_server.py" ${site:+--site "$site"} --prefix "$prefix" --catalog-host "$catalog_host" \
    --cert "$out/discord-cert.pem" --key "$out/discord-key.pem" --log "$out/discord-requests.log" \
    ${MAICORD_FAKE_MENTION_AFTER:+--mention-after "$MAICORD_FAKE_MENTION_AFTER"} \
    > "$out/discord-server.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null' EXIT
for _ in $(seq 50); do grep -q ready "$out/discord-server.log" 2>/dev/null && break; sleep 0.1; done
grep -q ready "$out/discord-server.log" || { cat "$out/discord-server.log" >&2; exit 1; }
export SSL_CERT_FILE="$out/discord-cert.pem"
if [ -n "${MAICORD_DISCORD_USER:-}" ]; then # the namespace is root's: the client runs as the user that asked for it
    sudo -u "$MAICORD_DISCORD_USER" -H -E env "PATH=$PATH" python3 -B "$here/client_flow.py" --binary "$binary" --out "$out" \
        "${flow_args[@]}" "${steps[@]}"
else
    python3 -B "$here/client_flow.py" --binary "$binary" --out "$out" "${flow_args[@]}" "${steps[@]}"
fi
status=$?
echo "requests:"; sort "$out/discord-requests.log" | uniq -c | sort -rn | head -60
cat "$out/discord-server.log" | tail -20
exit $status

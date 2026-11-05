#!/usr/bin/env bash
# Runs the Linux client against a local copy of the plugin catalog over real TLS and drives it on headless sway.
#
#   tools/test_catalog_wayland.sh --binary PATH --site DIR [--out DIR] [--host NAME] [--prefix PATH] [--expect TEXT]...
#                                 [--client-arg ARG]... STEP...
#
# The client reads the catalog at https://<owner>.github.io/<repo>/ with the key compiled into it, so the run
#   * builds a certificate for NAME (default maj113.github.io, the official owner) that the client trusts through
#     SSL_CERT_FILE (the Linux TLS client's trust override),
#   * enters a private user, network and mount namespace (unshare -Urnm: no root needed) in which NAME is 127.0.0.1 in a
#     private /etc/hosts and tools/catalog_site_server.py serves DIR on port 443,
#   * hands STEP... to tools/client_flow.py (wait:, click:, shot:, ... see its header), which starts sway and the client with
#     the same environment.
# The client must be built with the catalog's test key, which signed DIR (the build's cache variable
# MAICORD_PLUGIN_CATALOG_PUBLIC_KEY, or the same environment variable at the first configure); the repository stays the
# official one. Nothing leaves the machine. Output, in --out (default ~/mc-catalog-qa): client_flow's screenshots and logs
# and the server's (catalog-requests.log lists every path the client asked for). Each --expect TEXT must occur in that
# list (such as index-4.json or .mcfx), or the run fails.
#
# Needs unshare, openssl, python3, sway, swaymsg, grim, wtype (tools/client_flow.py).

set -uo pipefail

binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
site=""
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-catalog-qa}"
host="maj113.github.io"
prefix="/MaiCord-plugins"
steps=()
client_args=()
expect="${MAICORD_CATALOG_EXPECT:-}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --site) site="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --host) host="$2"; shift 2 ;;
        --prefix) prefix="$2"; shift 2 ;;
        --expect) expect+="$2"$'\n'; shift 2 ;;
        --client-arg) client_args+=("$2"); shift 2 ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) steps+=("$1"); shift ;;
    esac
done
reexec_args=()
flow_args=()
for value in "${client_args[@]}"; do # a value may look like an option (--url), so it is passed as --client-arg=VALUE
    reexec_args+=(--client-arg "$value")
    flow_args+=("--client-arg=$value")
done
[ -x "$binary" ] || { echo "No client at $binary" >&2; exit 1; }
[ -f "$site/index-4.json" ] || [ -f "$site/index-3.json" ] || { echo "No catalog site at '$site' (--site)" >&2; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

if [ -z "${MAICORD_CATALOG_NAMESPACE:-}" ]; then
    mkdir -p "$out"
    out="$(cd "$out" && pwd -P)"
    site="$(cd "$site" && pwd -P)"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj "/CN=$host" \
        -addext "subjectAltName=DNS:$host" -keyout "$out/catalog-key.pem" -out "$out/catalog-cert.pem" 2>/dev/null \
        || { echo "openssl could not make the certificate" >&2; exit 1; }
    printf '127.0.0.1 localhost %s\n' "$host" > "$out/catalog-hosts"
    exec unshare -Urnm env MAICORD_CATALOG_NAMESPACE=1 MAICORD_CATALOG_EXPECT="$expect" "$0" --binary "$binary" --site "$site" \
        --out "$out" --host "$host" \
        --prefix "$prefix" "${reexec_args[@]}" "${steps[@]}"
fi

# Inside the namespace: loopback, the private hosts file, the server.
ip link set lo up || { echo "no loopback in the namespace" >&2; exit 1; }
mount --bind "$out/catalog-hosts" /etc/hosts || { echo "could not replace /etc/hosts" >&2; exit 1; }
: > "$out/catalog-requests.log"
python3 -B "$here/catalog_site_server.py" --site "$site" --cert "$out/catalog-cert.pem" --key "$out/catalog-key.pem" \
    --prefix "$prefix" --log "$out/catalog-requests.log" > "$out/catalog-server.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null' EXIT
for _ in $(seq 50); do grep -q ready "$out/catalog-server.log" 2>/dev/null && break; sleep 0.1; done
grep -q ready "$out/catalog-server.log" || { cat "$out/catalog-server.log" >&2; exit 1; }
export SSL_CERT_FILE="$out/catalog-cert.pem"
python3 -B "$here/client_flow.py" --binary "$binary" --out "$out" "${flow_args[@]}" "${steps[@]}"
status=$?
echo "catalog requests:"; sort "$out/catalog-requests.log" | uniq -c | head -40
while IFS= read -r wanted; do
    [ -z "$wanted" ] && continue
    grep -q -- "$wanted" "$out/catalog-requests.log" || { echo "FAIL: the client never asked for $wanted" >&2; status=1; }
done <<< "$expect"
exit $status

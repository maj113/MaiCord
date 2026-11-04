#!/bin/sh
# tls_stream over the network layer against local `openssl s_server` processes (loopback only, no network).
#
#   tls_stream_smoke.sh <tls_stream test binary> <work directory>
#
# Makes a throwaway EC certificate for localhost and 127.0.0.1, starts TLS 1.3 and TLS 1.2 status-page servers and a TLS
# 1.3 file server on ports the kernel picks, and runs the cases of tests/platform/net/tls_stream_linux.cpp with that
# certificate as the only trust (SSL_CERT_FILE): a GET by name and by address over both versions, a 100,000 byte
# download over many records, a name the certificate does not carry, a port nobody listens on, and a stream closed
# while it connects. Exits 77 (skipped) when there is no `openssl` command.
set -u

binary=$1
work=$2

if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: no openssl command"
    exit 77
fi

rm -rf "$work"
mkdir -p "$work"
cd "$work" || exit 1

pids=""
failed=0

cleanup() {
    for pid in $pids; do
        kill "$pid" 2>/dev/null
    done
    for pid in $pids; do
        wait "$pid" 2>/dev/null
    done
    if [ "$failed" != 0 ]; then
        for log in server-*.log; do
            [ -f "$log" ] && { echo "--- $log"; cat "$log"; }
        done
    fi
    cd / && rm -rf "$work"
}
trap cleanup EXIT INT TERM

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout server.key -out server.pem -days 2 \
    -subj /CN=localhost -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 ||
    { echo "FAIL: cannot make the server certificate"; failed=1; exit 1; }
yes 0123456789abcdef | head -c 100000 > payload

# start_server <name> <s_server option>...: sets port_<name> to the port it listens on and pid_<name> to its process.
start_server() {
    name=$1
    shift
    openssl s_server -accept "${bind_address:-127.0.0.1}:0" -cert server.pem -key server.key -alpn http/1.1 -ign_eof "$@" \
        </dev/null >"server-$name.log" 2>&1 &
    pid=$!
    pids="$pids $pid"
    tries=0
    while ! grep -q '^ACCEPT' "server-$name.log" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -gt 100 ]; then
            echo "FAIL: s_server $name did not start"
            failed=1
            return 1
        fi
        sleep 0.05
    done
    port=$(sed -n 's/^ACCEPT .*:\([0-9][0-9]*\)$/\1/p' "server-$name.log" | head -n 1)
    eval "port_$name=$port"
    eval "pid_$name=$pid"
}

start_server www13 -www || exit 1
start_server www12 -www -tls1_2 || exit 1
start_server file13 -WWW || exit 1
# The same certificate answers on 127.0.0.2, an address it does not name (all of 127/8 is this machine).
bind_address=127.0.0.2 start_server other -www || exit 1
start_server gone -www || exit 1
kill "$pid_gone" 2>/dev/null
wait "$pid_gone" 2>/dev/null

export SSL_CERT_FILE="$work/server.pem"

# run <description> <arguments>...
run() {
    description=$1
    shift
    if ! "$binary" "$@"; then
        echo "FAIL: $description"
        failed=1
    fi
}

run "GET by name over TLS 1.3" --get "$port_www13" localhost TLSv1.3
run "GET by address over TLS 1.3" --get "$port_www13" 127.0.0.1 TLSv1.3
run "GET by name over TLS 1.2" --get "$port_www12" localhost TLSv1.2
run "download over TLS 1.3" --download "$port_file13" localhost 100000
run "address the certificate does not name" --reject "$port_other" 127.0.0.2
run "refused connection" --refused "$port_gone"
run "close while connecting" --cancel "$port_www13" localhost

[ "$failed" = 0 ] && echo "PASS maicord_tls_stream_smoke"
exit "$failed"

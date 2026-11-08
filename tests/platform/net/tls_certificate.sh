# Sourced by the scripts that serve TLS on this machine to a client that trusts only their certificate (POSIX sh):
# tests/platform/net/tls_smoke.sh, tls_stream_smoke.sh, tools/test_catalog_wayland.sh, tools/test_discord_wayland.sh.
#
# certificate <key file> <certificate file> <common name> [<subjectAltName>]: a P-256 key and a self-signed certificate
# under that subject for the names given (default DNS:localhost,IP:127.0.0.1), with the extensions `openssl req -x509`
# gives (CA:TRUE, key identifiers). It is valid from 2000 to 2099 rather than from now: the wall clock can step back
# while the tests run (WSL's time sync moved it 1.7 s back every 30 s under load), and a certificate dated now is then
# "not yet valid" (X509 verify result 9) to every client started in that window. `openssl req` can only date a
# certificate from now (before OpenSSL 3.4), so `openssl ca -selfsign` signs the request with these dates, from a
# throwaway CA database. tools/run_fake_server_client.ps1 makes the Windows client's certificate the same way.
certificate() {
    certificate_work=$(mktemp -d) || return 1
    cat > "$certificate_work/ca.cnf" <<EOF
[ca]
default_ca = selfsigned
[selfsigned]
database = $certificate_work/index.txt
new_certs_dir = $certificate_work
serial = $certificate_work/serial
default_md = sha256
policy = any_name
copy_extensions = copy
x509_extensions = authority
unique_subject = no
[any_name]
commonName = supplied
[authority]
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints = critical,CA:true
EOF
    : > "$certificate_work/index.txt"
    echo 01 > "$certificate_work/serial"
    openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$1" \
        -out "$certificate_work/request.csr" -subj "/CN=$3" -addext "subjectAltName=${4:-DNS:localhost,IP:127.0.0.1}" \
        >/dev/null 2>&1 &&
        openssl ca -batch -config "$certificate_work/ca.cnf" -selfsign -keyfile "$1" -in "$certificate_work/request.csr" \
            -out "$2" -notext -startdate 20000101000000Z -enddate 20991231235959Z >/dev/null 2>&1
    certificate_status=$?
    rm -rf "$certificate_work"
    return "$certificate_status"
}

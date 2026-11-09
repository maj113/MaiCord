"""Go Live loopback: MaiCord's sender streams to MaiCord's receive path through tools/fake_discord_server.py's stream server.

    tools/test_go_live_loopback.py LOOPBACK_EXE WORK_DIR [CODEC,...]

Each codec (H.264, H.265, AV1) has its own fake server (its UDP ports are ephemeral) with a throwaway certificate for its
stream server's name (made once with openssl, Git's on Windows) and its own LOOPBACK_EXE process (tests/app/voice/
go_live_loopback.cpp) pointed at it with the test hooks (MAICORD_TEST_SERVER, MAICORD_TEST_CA_FILE); the three run side
by side, as each is paced by the wall clock. It checks the server saw the DAVE group of the two and media both ways.
Exit 77 (skipped) without openssl or a hardware video device.
"""
import concurrent.futures
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = "us-qa-stream.discord.media"


def openssl():
    found = shutil.which("openssl")
    if found:
        return found
    git = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Git", "usr", "bin", "openssl.exe")
    return git if os.path.exists(git) else None


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def certificate(work, tool):
    cert, key = (os.path.join(work, name) for name in ("cert.pem", "key.pem"))
    if not os.path.exists(cert):
        subprocess.run([tool, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                        "-subj", f"/CN={HOST}", "-addext", f"subjectAltName=DNS:{HOST}", "-days", "30",
                        "-keyout", key, "-out", cert], check=True, capture_output=True)
    return cert, key


def run(exe, work, cert, key, codec):
    """(exit status, the lines to show) of one codec's server and client."""
    log = os.path.join(work, f"server-{codec}.log")
    open(log, "w").close()
    port = free_port()
    environment = dict(os.environ, MAICORD_FAKE_STREAM_CODEC={"h264": "H264", "h265": "H265", "av1": "AV1"}[codec])
    server = subprocess.Popen([sys.executable, "-B", os.path.join(HERE, "fake_discord_server.py"), "--cert", cert,
                               "--key", key, "--port", str(port), "--log", log],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
    try:
        first = server.stdout.readline().strip()
        if first != "ready":
            # Not "ready" means it exited (most often another fake server holds its UDP ports): show why.
            return 1, [f"{codec}: the fake server did not start\n{first}\n{server.stdout.read()}"]
        client = subprocess.run([exe], env=dict(os.environ, MAICORD_TEST_SERVER=f"127.0.0.1:{port}",
                                                MAICORD_TEST_CA_FILE=cert, MAICORD_LOOPBACK_CODEC=codec),
                                capture_output=True, text=True, timeout=45)
        shown = [f"{codec}: " + (client.stdout + client.stderr).strip()]
        if client.returncode == 77:
            return 77, shown
        with open(log, encoding="utf-8") as handle:
            lines = handle.read()
        wanted = [f"go live codec {codec.upper()}", "go live dave epoch 1 members 2", "go live rtp video packets 10\n",
                  "go live rtp audio packets 10", "go live rtcp packets 10", "go live video ssrc"]
        missing = [line for line in wanted if line not in lines]
        if missing:
            shown.append(f"{codec}: the stream server never logged: {missing}")
        return client.returncode or (1 if missing else 0), shown
    finally:
        server.terminate()
        server.wait(10)


def main():
    exe, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    tool = openssl()
    if not tool:
        print("go live loopback: no openssl; skipped")
        return 77
    started = time.time()
    codecs = sys.argv[3].split(",") if len(sys.argv) > 3 else ("h264", "h265", "av1")
    cert, key = certificate(work, tool)
    with concurrent.futures.ThreadPoolExecutor(len(codecs)) as pool:
        runs = list(pool.map(lambda codec: run(exe, work, cert, key, codec), codecs))
    for _, shown in runs:
        print("\n".join(shown))
    results = [result for result, _ in runs]
    print(f"go live loopback: {time.time() - started:.1f} s")
    if all(result == 77 for result in results):
        return 77
    return next((result for result in results if result not in (0, 77)), 0)


if __name__ == "__main__":
    sys.exit(main())

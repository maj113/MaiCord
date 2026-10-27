"""Bounded subprocess runner shared by the native-text QA scripts."""
import subprocess
import time


class CheckFailure(Exception):
    def __init__(self, row):
        self.row = row
        super().__init__(row['name'])


def run_check(name, command, cwd, logs, timeout, stop, *, env=None):
    before = time.perf_counter()
    log = logs / (name + '.log')
    status = 'passed'
    with log.open('w', encoding='utf-8') as output:
        if stop.is_set():
            status, code = 'cancelled', 130
        else:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output,
                stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            while process.poll() is None:
                if stop.is_set() or time.perf_counter()-before >= timeout:
                    status = 'cancelled' if stop.is_set() else 'timeout'
                    # Kill only this invocation's process tree, including any child it launched.
                    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                        stdout=output, stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NO_WINDOW, timeout=5)
                    process.wait(timeout=5)
                    break
                try:
                    process.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
            code = 124 if status == 'timeout' else 130 if status == 'cancelled' else process.returncode
            if code and status == 'passed':
                status = 'failed'
    row = dict(name=name, command=command, seconds=time.perf_counter()-before,
        exit=code, status=status, timeout_seconds=timeout, log=str(log))
    print(f"{name}: {row['seconds']:.2f}s, {status}, exit {code}", flush=True)
    if code:
        stop.set()
        if status != 'cancelled':
            print(log.read_text(encoding='utf-8', errors='replace')[-6000:])
        raise CheckFailure(row)
    return row

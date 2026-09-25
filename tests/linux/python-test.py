#!/usr/bin/python3
"""python-test: checks that Python works on Vexa's Linux subsystem: threads,
processes, shared memory, signals, files and a few standard modules. Prints
"python-test: passed" at the end, or what failed."""
import hashlib
import json
import mmap
import multiprocessing
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import zlib

failures = []


def check(name, condition):
    if not condition:
        failures.append(name)
        print("python-test: FAILED:", name, flush=True)
    else:
        print("python-test:", name, "ok", flush=True)


def worker(n):
    return n * n


def add_to(value, lock):
    for _ in range(100):
        with lock:
            value.value += 1


def main():
    # Threads sharing a lock.
    total = [0]
    lock = threading.Lock()

    def count():
        for _ in range(20000):
            with lock:
                total[0] += 1

    threads = [threading.Thread(target=count) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check("threads", total[0] == 80000)

    # Another program, through a pipe.
    out = subprocess.run(["echo", "hello"], capture_output=True, text=True)
    check("subprocess", out.stdout == "hello\n" and out.returncode == 0)

    # Processes, with semaphores and shared memory underneath.
    with multiprocessing.Pool(2) as pool:
        check("multiprocessing pool", pool.map(worker, range(5)) == [0, 1, 4, 9, 16])
    shared = multiprocessing.Value("i", 0)
    shared_lock = multiprocessing.Lock()
    procs = [multiprocessing.Process(target=add_to, args=(shared, shared_lock)) for _ in range(3)]
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    check("shared value", shared.value == 300)

    # Anonymous shared memory across fork.
    memory = mmap.mmap(-1, 4096)
    pid = os.fork()
    if pid == 0:
        memory[:6] = b"forked"
        os._exit(0)
    os.waitpid(pid, 0)
    check("shared mmap", memory[:6] == b"forked")

    # A signal handler, from a timer.
    got = []
    signal.signal(signal.SIGALRM, lambda signum, frame: got.append(signum))
    signal.setitimer(signal.ITIMER_REAL, 0.1)
    time.sleep(0.5)
    check("timer signal", got == [signal.SIGALRM])

    # Files and modules.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "data.json")
        with open(path, "w") as f:
            json.dump({"vexa": [1, 2, 3]}, f)
        with open(path) as f:
            check("json file", json.load(f) == {"vexa": [1, 2, 3]})
        os.link(path, path + ".link")
        check("hard link", os.stat(path).st_nlink == 2)
    check("zlib", zlib.decompress(zlib.compress(b"x" * 1000)) == b"x" * 1000)
    check("hashlib", hashlib.sha256(b"abc").hexdigest().startswith("ba7816bf"))

    if failures:
        print("python-test: FAILED:", ", ".join(failures))
        return 1
    print("python-test: passed (Python", sys.version.split()[0] + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())

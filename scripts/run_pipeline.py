#!/usr/bin/env python3
"""Supervise a local pipeline with isolated ring names and readiness handshakes."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

def wait_ring(name, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("ring owner exited during startup")
        try:
            with open("/dev/shm/" + name.lstrip("/"), "rb") as f:
                fcntl.flock(f, fcntl.LOCK_SH | fcntl.LOCK_NB)
                if os.fstat(f.fileno()).st_size:
                    return
        except (FileNotFoundError, BlockingIOError):
            pass
        time.sleep(.01)
    raise TimeoutError("ring readiness timed out")

def run(args):
    processes = []
    prefix = "pulse_" + uuid.uuid4().hex
    rings = ["/" + prefix + "_in", "/" + prefix + "_out"]
    bin_dir = Path(args.bin_dir).resolve()
    def start(app, *options):
        p = subprocess.Popen([str(bin_dir / app), *map(str, options)])
        processes.append(p)
        return p
    try:
        receiver = start("receiver", "--allow-insecure-udp", "--out-shm", rings[1], "--slots", args.slots,
                         "--bind", "127.0.0.1", "--port", args.port,
                         "--count", args.count, "--idle-ms", 10000, "--wait-readers", 1)
        wait_ring(rings[1], receiver)
        start("consumer", "--shm", rings[1], "--slots", args.slots,
              "--count", args.count, "--idle-ms", 10000)
        producer = start("producer", "--shm", rings[0], "--slots", args.slots,
                         "--count", args.count, "--rate", args.rate, "--wait-readers", 1)
        wait_ring(rings[0], producer)
        start("sender", "--allow-insecure-udp", "--in-shm", rings[0], "--slots", args.slots,
              "--dst", f"127.0.0.1:{args.port}", "--count", args.count,
              "--fec-k", args.fec_k, "--fec-parity", args.fec_parity,
              "--idle-ms", 10000)
        # The state file is local discovery, not a distributed registry.
        if args.state_file:
            state = Path(args.state_file)
            temporary = state.with_name(state.name + "." + prefix)
            temporary.write_text(json.dumps({"epoch": prefix, "rings": rings,
                                            "pids": [p.pid for p in processes]}))
            os.replace(temporary, state)
        deadline = time.monotonic() + args.timeout
        while any(p.poll() is None for p in processes):
            if any(p.poll() not in (None, 0) for p in processes):
                raise RuntimeError("pipeline process failed")
            if time.monotonic() > deadline:
                raise TimeoutError("pipeline deadline exceeded")
            time.sleep(.02)
        if any(p.returncode for p in processes):
            raise RuntimeError("pipeline process failed")
    finally:
        for p in processes:
            if p.poll() is None:
                p.terminate()
        for p in processes:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        # These unpredictable names belong only to this run.
        for name in rings:
            Path("/dev/shm/" + name.lstrip("/")).unlink(missing_ok=True)
        if args.state_file:
            state = Path(args.state_file)
            if state.exists() and json.loads(state.read_text()).get("epoch") == prefix:
                state.unlink()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bin-dir", default="build/bin")
    p.add_argument("--count", type=int, default=5000)
    p.add_argument("--slots", type=int, default=8192)
    p.add_argument("--rate", type=float, default=25000)
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--fec-k", type=int, default=0)
    p.add_argument("--fec-parity", type=int, default=1)
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--restarts", type=int, default=0,
                   help="restart the whole group with a new epoch on failure")
    p.add_argument("--state-file", help="atomic local discovery JSON file")
    args = p.parse_args()
    if args.timeout <= 0 or args.restarts < 0 or args.rate < 0:
        p.error("timeout must be positive; restarts and rate nonnegative")
    if args.count < 1 or args.slots < 1 or args.slots & (args.slots - 1):
        p.error("count must be positive and slots a power of two")
    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    for attempt in range(args.restarts + 1):
        try:
            run(args)
            return
        except KeyboardInterrupt:
            p.exit(130, "pipeline stopped\n")
        except (OSError, RuntimeError, TimeoutError) as error:
            if attempt == args.restarts:
                p.exit(1, f"pipeline failed: {error}\n")
            time.sleep(.5)

if __name__ == "__main__":
    main()

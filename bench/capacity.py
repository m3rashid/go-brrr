#!/usr/bin/env python3

"""Capacity probe: hold N idle keep-alive connections open against brrr and
read each worker's memory from GET /_stats. Intended to run *inside* the
docker network (host = service name), so the client is under the same
envelope as the server."""
import socket, sys, time, json, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
TARGET = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
STEP = int(sys.argv[4]) if len(sys.argv) > 4 else 2000

def stats():
    """per-worker (pid -> rss bytes, conns) + max cgroup-wide memory (bytes).
    Every request may land on a different worker behind SO_REUSEPORT, so poll
    a few times and keep the max seen per pid."""
    seen = {}
    cgroup_max = 0
    for _ in range(6):
        try:
            with urllib.request.urlopen(f"http://{HOST}:{PORT}/_stats", timeout=2) as r:
                d = json.loads(r.read())
            pid = d["pid"]
            prev = seen.get(pid, (0, 0))
            seen[pid] = (max(prev[0], d["rss_kb"] * 1024), max(prev[1], d["conns"]))
            cgroup_max = max(cgroup_max, d.get("cgroup_kb", 0) * 1024)
        except Exception:
            pass
        time.sleep(0.02)
    return seen, cgroup_max

socks = []
ok = 0
START = int(sys.argv[5]) if len(sys.argv) > 5 else 0  # run several probes: split the range
try:
    while ok < TARGET - START:
        n = min(STEP, TARGET - START - ok)
        for _ in range(n):
            s = socket.create_connection((HOST, PORT), timeout=5)
            s.sendall(b"GET / HTTP/1.1\r\nHost: b\r\n\r\n")
            s.recv(4096)  # finish one request; connection becomes idle keep-alive
            socks.append(s)
        ok += n
        time.sleep(0.3)
        seen, cg = stats()
        rss = sum(v[0] for v in seen.values())
        conns = sum(v[1] for v in seen.values())
        print(f"users: {START + ok:6d}   live conns: {conns:6d}   "
              f"brrr VmRSS: {rss/1048576:7.1f} MB   cgroup: {cg/1048576:7.1f} MB   "
              f"({rss/max(START+ok,1):,.0f} B/user)", flush=True)
    print(f"target reached ({TARGET} users) without hitting a wall")
except (OSError, socket.timeout) as e:
    print(f"wall hit at {START + ok} users: {e!r}")
    # hold the sockets so ground truth can be sampled from the server
    import os
    hold = int(os.environ.get("HOLD_SECONDS", "0"))
    if hold:
        print(f"holding {len(socks)} sockets for {hold}s...", flush=True)
        time.sleep(hold)
finally:
    for s in socks:
        try: s.close()
        except OSError: pass

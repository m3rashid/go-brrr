#!/usr/bin/env python3
"""Simple concurrency benchmark for brrr (keep-alive, epoll-friendlier than ab)."""
import asyncio, os, sys, time

HOST = os.environ.get("BENCH_HOST", "127.0.0.1")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
TOTAL = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
CONC = int(sys.argv[3]) if len(sys.argv) > 3 else 200

REQ = (b"GET /users HTTP/1.1\r\nHost: b\r\nConnection: keep-alive\r\n\r\n")

async def worker(n_reqs, stats):
    reader, writer = await asyncio.open_connection(HOST, PORT)
    sock = writer.get_extra_info("socket")
    try:
        for _ in range(n_reqs):
            t0 = time.perf_counter()
            writer.write(REQ)
            await writer.drain()
            # read status line + headers + body
            clen = 0
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b""):
                    break
                if line.startswith(b"Content-Length:"):
                    clen = int(line.split(b":")[1])
            body = await reader.readexactly(clen)
            dt = time.perf_counter() - t0
            stats["lat"] += dt
            stats["max"] = max(stats["max"], dt)
            stats["ok"] += 1
    except Exception as e:
        stats["err"] += 1
        print("worker error:", e)
    finally:
        writer.close()

async def main():
    stats = {"ok": 0, "err": 0, "lat": 0.0, "max": 0.0}
    per = TOTAL // CONC
    t0 = time.perf_counter()
    await asyncio.gather(*[worker(per, stats) for _ in range(CONC)])
    dt = time.perf_counter() - t0
    n = stats["ok"]
    print(f"requests: {n} ok, {stats['err']} errors")
    print(f"concurrency: {CONC}   time: {dt:.2f}s")
    print(f"req/sec: {n/dt:,.0f}")
    if n:
        print(f"avg latency: {stats['lat']/n*1000:.2f} ms   max: {stats['max']*1000:.2f} ms")

asyncio.run(main())

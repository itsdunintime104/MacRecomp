"""Summarise a HPRECOMP_FRAME_TRACE file (format: runtime/include/frame_trace.h).

Usage: ft_report.py TRACE [--phase NAME:T0-T1 ...] [--list MS]
Per phase (times in seconds): frames, median and p99 frame time, frames over
16.7 ms and 33.3 ms, and the cause of each frame over 33.3 ms (--list MS
lowers that bar). Cause rules, on the thread that calls Present:
  call:<name>   one wrapped host call took half the frame or more
  host          wrapped host calls took half the frame or more together
  offcpu        outside host calls, the thread ran under half that time
  guest         outside host calls, the thread was mostly on the CPU
Slow calls (S lines) on any thread that overlap the frame are listed with it.
"""

import argparse
import statistics
from collections import Counter
from dataclasses import dataclass


@dataclass
class Frame:
    n: int
    t: float  # ms, at the Present entry that ends the frame
    dt: float
    cpu: float
    host: float
    max_ms: float
    max_name: str
    present: float


@dataclass
class Slow:
    t: float
    tid: str
    thread: str
    name: str
    ms: float


def parse(path: str) -> tuple[list[Frame], list[Slow]]:
    frames: list[Frame] = []
    slows: list[Slow] = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) == 9 and p[0] == "F":
                frames.append(Frame(int(p[1]), float(p[2]), float(p[3]), float(p[4]),
                                    float(p[5]), float(p[6]), p[7], float(p[8])))
            elif len(p) == 6 and p[0] == "S":
                slows.append(Slow(float(p[1]), p[2], p[3], p[4], float(p[5])))
    return frames, slows


def cause(fr: Frame) -> str:
    if fr.max_ms >= 0.5 * fr.dt:
        return "call:" + fr.max_name
    if fr.host >= 0.5 * fr.dt:
        return "host"
    outside = fr.dt - fr.host
    return "offcpu" if fr.cpu < 0.5 * outside else "guest"


def p99(xs: list[float]) -> float:
    s = sorted(xs)
    return s[min(len(s) - 1, int(0.99 * len(s)))]


def report(name: str, frames: list[Frame], slows: list[Slow], bar: float) -> None:
    if not frames:
        print(f"{name}: no frames")
        return
    dts = [f.dt for f in frames]
    over16 = sum(1 for d in dts if d > 16.7)
    over33 = sum(1 for d in dts if d > 33.3)
    print(f"{name}: frames {len(dts)} median {statistics.median(dts):.2f} ms "
          f"p99 {p99(dts):.2f} ms >16.7 {over16} >33.3 {over33}")
    spikes = [f for f in frames if f.dt > bar]
    print(f"  causes over {bar} ms: {dict(Counter(cause(f) for f in spikes))}")
    for f in spikes:
        t0 = f.t - f.dt
        hits = [s for s in slows if s.t < f.t and s.t + s.ms > t0]
        extra = "; ".join(f"{s.thread}/{s.tid} {s.name} {s.ms:.1f}" for s in hits[:4])
        print(f"  t={f.t / 1000:8.3f}s dt={f.dt:7.1f} cpu={f.cpu:6.1f} host={f.host:6.1f} "
              f"max={f.max_ms:6.1f} {f.max_name} -> {cause(f)} | {extra}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--phase", action="append", default=[], help="NAME:T0-T1 in seconds")
    ap.add_argument("--list", type=float, default=33.3)
    a = ap.parse_args()
    frames, slows = parse(a.trace)
    phases: list[tuple[str, float, float]] = []
    for ph in a.phase:
        nm, rng = ph.split(":")
        lo, hi = rng.split("-")
        phases.append((nm, float(lo) * 1000, float(hi) * 1000))
    if not phases:
        phases = [("all", 0.0, float("inf"))]
    for nm, lo, hi in phases:
        report(nm, [f for f in frames if lo <= f.t < hi], slows, a.list)
    calls = Counter(s.name for s in slows)
    print("slow calls (>= 2 ms) by name:", calls.most_common(15))


if __name__ == "__main__":
    main()

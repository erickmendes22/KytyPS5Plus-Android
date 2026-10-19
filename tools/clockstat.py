#!/usr/bin/env python3
"""tools/clockstat.py <run dir>... [--region HASH]...

GPU clock state per phase of an Astro bench run (research/clock_policy.md). For the bench window
and every live-knob sweep phase (tools/r_astro_bench.sh SWEEP=...) it prints the [perf-probe]
median (4.547 ms = full clock, ~6.01 ms = the throttled DCVS level), how many probe samples were
at full clock (<= 4.60 ms), fps, GPU ms/frame ([perf-wait] gpu_busy) and that GPU time normalized
by the phase's mean probe. Phases with fewer than 85% of their probe samples at full clock are
marked THROTTLED and must not be used for an A/B. --region <hex prefix> adds that region's ms/frame (from the [perf-gpu0] lines;
needs KYTY_PERF_GPU_TOP large enough to list it). The first 3 s of each phase are dropped.
"""
import gzip
import re
import statistics
import sys
from pathlib import Path

FULL_CLOCK_MS = 4.547
FULL_LIMIT_MS = 4.60
SKIP_S = 3
REGION = re.compile(r"\[([^\]]+)\]=([\d.]+)\((\d+)\)")


def new_phase(name):
    return {"name": name, "secs": 0, "probe": [], "fps": [], "gpu": [], "regions": {}, "gpu0": 0}


def scan(run: Path, regions):
    path = run / "_perf_log.txt.gz"
    if not path.exists():
        path = run / "_perf_log.txt"
    if not path.exists():
        return []
    opener = gzip.open if path.suffix == ".gz" else open
    window = None
    if (run / "BENCH_WINDOW").exists():
        w = (run / "BENCH_WINDOW").read_text().split()
        window = (int(w[0]), int(w[1]))
    win = new_phase("window")
    phases = [win]
    cur = None
    sec = 0
    for line in opener(path, "rt", errors="replace"):
        if line.startswith("[live-knobs]") and window and sec >= window[1]:
            name = re.sub(r"^\[live-knobs\] t=[\d.]+ ", "", line.strip())
            name = name.replace("mesh-skip-draw=[] mesh-skip-cs=[] ", "")
            cur = new_phase(name)
            phases.append(cur)
            continue
        targets = []
        if window and window[0] <= sec < window[1]:
            targets.append(win)
        if cur is not None and cur["secs"] > SKIP_S:
            targets.append(cur)
        if line.startswith("[perf] "):
            sec += 1
            if cur is not None:
                cur["secs"] += 1
            fps = float(re.search(r"flips/s=([\d.]+)", line).group(1))
            for p in targets:
                p["fps"].append(fps)
        elif line.startswith("[perf-probe] t="):
            value = float(re.search(r"gpu_ms=([\d.]+)", line).group(1))
            for p in targets:
                p["probe"].append(value)
        elif line.startswith("[perf-wait] per frame: gpu_busy="):
            value = float(re.search(r"gpu_busy=([\d.]+)", line).group(1))
            for p in targets:
                p["gpu"].append(value)
        elif line.startswith("[perf-gpu0]"):
            found = {name: float(ms) for name, ms, _ in REGION.findall(line)}
            for p in targets:
                p["gpu0"] += 1
                for prefix in regions:
                    ms = sum(v for k, v in found.items() if k.split()[-1][2:].startswith(prefix))
                    p["regions"].setdefault(prefix, []).append(ms)
    return [p for p in phases if p["fps"] or p["probe"]]


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    regions = []
    while "--region" in args:
        i = args.index("--region")
        regions.append(args[i + 1].lower().removeprefix("0x"))
        del args[i:i + 2]
    for run in args:
        print(f"== {Path(run).name}")
        for p in scan(Path(run), regions):
            probe = statistics.median(p["probe"]) if p["probe"] else float("nan")
            full = sum(1 for v in p["probe"] if v <= FULL_LIMIT_MS)
            fps, gpu = mean(p["fps"]), mean(p["gpu"])
            # Usable for an A/B only when (nearly) every probe sample was at full clock.
            state = "ok" if p["probe"] and full >= 0.85 * len(p["probe"]) else "THROTTLED"
            extra = ""
            for prefix in regions:
                ms_s = mean(p["regions"].get(prefix, []))
                extra += f"  {prefix[:8]} {ms_s / fps if fps else float('nan'):5.2f}"
            print(f"  {p['name'][:56]:<56} probe {probe:5.3f} ({full}/{len(p['probe'])} full) "
                  f"{state:<9} fps {fps:5.2f}  GPU {gpu:5.1f} ms/f  "
                  f"@full {gpu * FULL_CLOCK_MS / mean(p['probe']):5.1f}{extra}")


if __name__ == "__main__":
    main()

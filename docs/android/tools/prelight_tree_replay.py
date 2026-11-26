#!/usr/bin/env python3
"""Offline N00 evidence reader. Never turns an unverified dark pixel into a bug.
Usage: python prelight_tree_replay.py diagnostics.zip [--oracle expected.json]
Oracle: [{"consumer_seq": int, "start_seq": int, "point": 0..63,
          "expected_word": int, "provenance": nonempty string,
          "same_cycle_certified": true, "history_certified": true}]
Certification must come from the guest contract/replay, not color counts.
The script rejects missing grids, changed native owner/subresource, sequence
gaps and incomplete before/after pairs. Cache events are candidate writers.
"""
import argparse
import json
import zipfile
from collections import Counter, defaultdict
from pathlib import PurePosixPath

def read_zip(path):
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if PurePosixPath(n).name.startswith("ds_prelight_events_") and n.endswith(".jsonl")]
        names.sort(key=lambda n: int(PurePosixPath(n).stem.rsplit("_",1)[1]))
        rows, errors = [], []
        for n in names:
            for i,line in enumerate(z.read(n).splitlines(),1):
                try:
                    rows.append(json.loads(line))
                except (ValueError, UnicodeError):
                    errors.append(f"{n}:{i}")
        stats = [n for n in z.namelist() if n.endswith("/ds_prelight_status.json")]
        status = json.loads(z.read(stats[0])) if len(stats)==1 else {}
    return rows, errors, status

def identity(row):
    im = row.get("image",{})
    view = row.get("view",{})
    return tuple(im.get(k) for k in ("owner","object","native","format","width","height")) + (
        view.get("mip",row.get("mip",0)),view.get("layer",row.get("layer",0)))

def grid(row):
    v = row.get("grid")
    return v if isinstance(v,list) and len(v)==64 and all(isinstance(x,int) for x in v) else None

def analyze(rows, errors, status, oracle=()):
    by_seq = {r["seq"]:r for r in rows}
    seqs = [r["seq"] for r in rows]
    counts = Counter(r.get("kind") for r in rows)
    gaps = [(a,b) for a,b in zip(seqs,seqs[1:]) if b!=a+1]
    pairs = defaultdict(dict)
    for r in rows:
        if r.get("kind") in ("draw_before","draw_after","draw_execute"):
            pairs[r["draw"]][r["kind"]]=r
    missing_pairs = [k for k,p in pairs.items() if "draw_before" in p and (
        not grid(p["draw_before"]) or not grid(p.get("draw_after",{})))]
    candidates = []
    for k,p in pairs.items():
        before,after = p.get("draw_before",{}),p.get("draw_after",{})
        if grid(before) and grid(after) and identity(before)==identity(after):
            lost = [i for i,(a,b) in enumerate(zip(grid(before),grid(after))) if a&0xffffff and not b&0xffffff]
            if lost:
                candidates.append({"draw":k,"frame":after["frame"],"after_seq":after["seq"],
                    "points":lost,"load_clear":p.get("draw_execute",{}).get("load_clear")})
    consumers = [r for r in rows if r.get("kind")=="consumer" and r.get("full")]
    decisions = []
    for o in oracle:
        c=by_seq.get(o["consumer_seq"],{})
        start,end=o["start_seq"],o["consumer_seq"]
        p,w=o["point"],o["expected_word"]
        verdict={"consumer_seq":end,"point":p,"node":"N00","decision":"BLOCKED"}
        if not (0<=p<64 and start<=end and grid(c) and c.get("kind")=="consumer"
                and o.get("provenance") and o.get("same_cycle_certified") is True
                and o.get("history_certified") is True):
            verdict["reason"]="missing independently certified expectation/history"
        elif errors or any(i not in by_seq for i in range(start,end+1)):
            verdict["reason"]="sequence gap or parse error"
        elif grid(c)[p]==w:
            verdict["reason"]="consumer already matches expected; witness is not a failure"
        else:
            observed=[r for r in rows if start<=r["seq"]<=end and r.get("kind") in
                      ("draw_before","draw_after","compute_before","compute_after","consumer")]
            # Require exact owner/native/extent/subresource and complete sampled
            # operations, never use another view or missing sample as absence.
            comparable=[r for r in observed if identity(r)==identity(c)]
            if not comparable or any(not grid(r) for r in comparable) or any(
                k in missing_pairs for k in (r.get("draw") for r in comparable)):
                verdict["reason"]="incomplete comparable samples"
            else:
                hits=[r["seq"] for r in comparable if grid(r)[p]==w]
                verdict["decision"]="S" if hits else "N"
                verdict["next"]="N10" if hits else "N20"
                verdict["matching_seq"]=hits
        decisions.append(verdict)
    return {"node":"N00","status":status,"events":len(rows),"kinds":dict(counts),
            "sequence_gaps":gaps,"parse_errors":errors,"missing_draw_pairs":missing_pairs,
            "consumer_full_series":[{"seq":r["seq"],"frame":r["frame"],"queue":r["queue"],
                "image":r["image"],"full":r["full"]} for r in consumers],
            "candidate_draw_losses":candidates,"decisions":decisions,
            "default_decision":"BLOCKED until an expected word is independently established"}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zip")
    ap.add_argument("--oracle")
    args=ap.parse_args()
    rows,errors,status=read_zip(args.zip)
    oracle=[]
    if args.oracle:
        with open(args.oracle,encoding="utf-8") as f:
            oracle=json.load(f)
    print(json.dumps(analyze(rows,errors,status,oracle),indent=2))

if __name__=="__main__":
    main()

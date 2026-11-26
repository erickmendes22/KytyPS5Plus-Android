#!/usr/bin/env python3
"""Replay N20 command admission/indirect fixup and join N30 original coverage.
The shader contract is indirect_draw_fixup.comp mode 0. A mismatch is a
candidate until recorded inputs are certified to be the actual consumed
bytes (same BDA and last writer, no intervening modification).
No inference of expected visibility from a zero occlusion count.
"""
import argparse
import collections
import json
import pathlib
import zipfile

def expected(src, stride, count, indexed, limit, active_count):
    width = 5 if indexed else 4
    if stride % 4 or stride // 4 < width or count < 1:
        raise ValueError("invalid stride/count")
    if len(src) != (count - 1) * (stride // 4) + width:
        raise ValueError("incomplete source")
    if not all(isinstance(w, int) and 0 <= w <= 0xffffffff for w in src):
        raise ValueError("invalid source word")
    output = []
    for i in range(count):
        if i >= min(active_count, count):
            output.extend([0] * width)
            continue
        start = i * (stride // 4)
        w = src[start:start+width]
        if indexed:
            w[0] = min(w[0], limit - min(w[2], limit))
        output.extend(w)
    return output

def read(path):
    with zipfile.ZipFile(path) as z:
        names = sorted((n for n in z.namelist()
            if pathlib.PurePosixPath(n).name.startswith("ds_prelight_events_")
            and n.endswith(".jsonl")), key=lambda n: int(n.rsplit("_",1)[1].split(".")[0]))
        events = [json.loads(l) for n in names for l in z.read(n).splitlines()]
        coverage = [json.loads(z.read(n)) for n in z.namelist()
            if pathlib.PurePosixPath(n).name.startswith("ds_geometry_coverage_f") and n.endswith(".json")]
        positions = [json.loads(z.read(n)) for n in z.namelist()
            if pathlib.PurePosixPath(n).name.startswith("ds_vertex_position_") and n.endswith(".json")]
        stat = next((json.loads(z.read(n)) for n in z.namelist()
            if n.endswith("/ds_prelight_status.json")),{})
    return events,coverage,positions,stat

def analyze(events, coverage, positions, stat):
    commands = collections.defaultdict(list)
    for e in events:
        cmd=e.get("command")
        if cmd: commands[cmd].append(e)
        if e["kind"]=="guest_command": commands[e["seq"]].append(e)
    fixups=[]
    returns=collections.Counter()
    incomplete=[]
    for cmd,es in commands.items():
        ret=[e for e in es if e["kind"]=="command_return"]
        if not ret: incomplete.append(cmd)
        for e in ret: returns[e["outcome"]]+=1
        for f in (e for e in es if e["kind"]=="indirect_fixed"):
            report={"command":cmd,"seq":f["seq"],"status":"BLOCKED"}
            inputs=[e for e in es if e["kind"]=="indirect_source"
                and e["seq"]<f["seq"] and e["bda"]==f["source_bda"]
                and e["args_guest"]==f["args_guest"]]
            counts=[e for e in es if e["kind"]=="indirect_count"
                and e["seq"]<f["seq"] and e["bda"]==f["count_bda"]]
            if not inputs or (f["count_bda"] and not counts):
                report["reason"]="missing matching BDA input/count"
            else:
                s=inputs[-1]
                c=counts[-1]["words"] if f["count_bda"] else [f["max_count"]]
                report["input_seq"]=s["seq"]
                report["count_seq"]=counts[-1]["seq"] if f["count_bda"] else None
                try:
                    if len(c)!=1 or s["max_count"]!=f["max_count"] or s["index_limit"]!=f["index_limit"]:
                        raise ValueError("incomplete count or parameter mismatch")
                    w=expected(s["words"],s["stride"],s["max_count"],s["indexed"],s["index_limit"],c[0])
                    a=f["words"]
                    if len(a)!=len(w):
                        raise ValueError("incomplete output")
                    report.update(status="MATCH" if a==w else "MISMATCH_CANDIDATE",
                        expected=w,observed=a,producer_shader=s.get("producer_shader"),
                        producer_tick=s.get("producer_tick"))
                except (ValueError,KeyError) as e:
                    report["reason"]=str(e)
            fixups.append(report)
    cov=[]
    for c in coverage:
        for d in c["draws"]:
            if d.get("kind")!="original": continue
            cov.append({k:d.get(k) for k in ("journal_draw","command","frame","submit","vs","ps",
                "index_guest","index_bytes","index_count_or_records","instances","indirect_guest",
                "depth_target","gbuffer_layer0","passed_samples")})
    paired=collections.defaultdict(list)
    for v in positions: paired[v.get("pair_id")].append(v)
    pairs=[]
    for k,v in paired.items():
        pre=[x for x in v if not x.get("material")]
        mat=[x for x in v if x.get("material")]
        p={"pair_id":k,"status":"BLOCKED"}
        if len(pre)==len(mat)==1:
            a,b=pre[0],mat[0]
            p.update(pre_frame=a["frame"],mat_frame=b["frame"],
                pre_draw=a.get("journal_draw"),mat_draw=b.get("journal_draw"),
                index_identity=all(a.get(x)==b.get(x) for x in ("index_guest","indices","base_vertex","first_instance")),
                missing_vertices=[a.get("missing"),b.get("missing")])
            p["status"]="PAIR_AVAILABLE" if p["index_identity"] else "IDENTITY_MISMATCH"
        pairs.append(p)
    seq=[e["seq"] for e in events]
    return {"journal_status":stat,"events":len(events),
        "sequence_gaps":[[a,b] for a,b in zip(seq,seq[1:]) if b!=a+1],
        "command_returns":dict(returns),"incomplete_commands":incomplete,
        "fixups":fixups,"coverage":cov,"position_pairs":pairs,
        "note":"No proof of expected visibility or root cause from counts alone."}

def selftest():
    assert expected([20,2,8,0xffffffff,7],20,1,True,12,1)==[4,2,8,0xffffffff,7]
    assert expected([20,1,15,0,0],20,1,True,12,1)==[0,1,15,0,0]
    assert expected([3,2,1,4,999,5,3,2,1],20,2,False,0,1)==[3,2,1,4,0,0,0,0]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("zip",nargs="?")
    p.add_argument("--selftest",action="store_true")
    a=p.parse_args()
    if a.selftest:selftest()
    if a.zip: print(json.dumps(analyze(*read(a.zip)),indent=2))
if __name__=="__main__": main()

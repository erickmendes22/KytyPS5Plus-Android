#!/usr/bin/env python3
"""Build a GFXReconstruct transfer replay plan from real debug markers and
compare full captured boundary images. Never promote same-SPIR-V execution
to a guest-ISA certificate. Missing or ambiguous evidence is BLOCKED.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import zipfile

def require(ok, message):
    if not ok:
        raise ValueError(message)

def read_jsonl(p):
    with open(p, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1024*1024), b""):
            h.update(b)
    return h.hexdigest()

def extract_capture(zpath, dest):
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        prefix = "p8-reference/"
        m = json.loads(z.read(prefix+"export-manifest.json"))
        names = set()
        for item in m["files"]:
            n = item["file"]
            require(Path(n).name == n and n not in names, "unsafe/duplicate file")
            names.add(n)
            require(item["status"]=="complete" and item["bytes"]==item["copied"], "incomplete export: "+n)
            info = z.getinfo(prefix+n)
            require(info.file_size==item["bytes"], "invalid size: "+n)
            with z.open(info) as src, open(dest/n, "wb") as out:
                shutil.copyfileobj(src, out, 1024*1024)
            require(sha(dest/n)==item["sha256"], "hash mismatch: "+n)
    traces = list(dest.glob("*.gfxr"))
    require(len(traces)==1 and traces[0].stat().st_size>0, "missing/ambiguous replay stream")
    return str(traces[0])

def native_plan(events, commands):
    snaps = [e for e in events if e.get("kind")=="snapshot"]
    require(snaps, "no full boundary snapshots")
    require(all(e.get("valid") for e in snaps), "invalid snapshot(s)")
    by_marker = {e["marker"]:e for e in snaps}
    require(len(by_marker)==len(snaps), "duplicate snapshot marker")
    active, pending, found, submitted = {}, {}, {}, {}
    generations = {}
    for entry in commands:
        f=entry.get("function")
        if not isinstance(f,dict):
            continue
        i=entry.get("index")
        name=f.get("name")
        args=f.get("args") or {}
        cb=args.get("commandBuffer")
        if name=="vkBeginCommandBuffer":
            require(cb is not None and i is not None, "invalid begin")
            active[cb]=i
            pending.pop(cb,None)
            generations.setdefault(cb,[]).append(i)
        elif name=="vkCmdInsertDebugUtilsLabelEXT":
            label=(args.get("pLabelInfo") or {}).get("pLabelName")
            if label not in by_marker:
                continue
            require(cb in active and cb not in pending and label not in found, "ambiguous marker: "+label)
            pending[cb]=label
        elif name=="vkCmdCopyImageToBuffer" and cb in pending:
            label=pending.pop(cb)
            e=by_marker[label]
            regions=args.get("pRegions") or []
            require(len(regions)==1 and args.get("regionCount",1)==1,"ambiguous copy: "+label)
            region=regions[0]
            sub=region["imageSubresource"]
            extent=region["imageExtent"]
            require([extent["width"],extent["height"]]==e["extent"] and extent["depth"]==1,
                    "copy extent mismatch: "+label)
            require(sub["mipLevel"]==e["mip"] and sub["baseArrayLayer"]==e["layer"] and sub["layerCount"]==1,
                    "subresource mismatch: "+label)
            require(region.get("bufferRowLength",0)==0 and region.get("bufferImageHeight",0)==0,
                    "non-tightly-packed copy: "+label)
            found[label]={"command":i,"begin":active[cb],"cb":cb,"snapshot":e}
        elif name in ("vkQueueSubmit","vkQueueSubmit2","vkQueueSubmit2KHR"):
            for submit in args.get("pSubmits") or []:
                cbs=submit.get("pCommandBuffers")
                if cbs is None:
                    cbs=[p["commandBuffer"] for p in submit.get("pCommandBufferInfos",[])]
                for c in cbs:
                    key=(c,active.get(c))
                    submitted.setdefault(key,[]).append(i)
    require(set(found)==set(by_marker), "missing capture markers: "+str(sorted(set(by_marker)-set(found))))
    require(not pending, "unfinished marker")
    groups={}
    for label,e in found.items():
        submit=submitted.get((e["cb"],e["begin"]),[])
        require(len(submit)==1 and submit[0]>e["command"],"missing/reused submit: "+label)
        e["submit_index"]=submit[0]
        groups.setdefault((submit[0],e["begin"]),[]).append(e["command"])
    keys=sorted(groups)
    plan={"BeginCommandBuffer":[k[1] for k in keys],
          "QueueSubmit":[k[0] for k in keys],
          "Transfer":[sorted(groups[k]) for k in keys]}
    return plan,found

def transfer_entries(data):
    if isinstance(data,list):
        for block in data:
            yield from transfer_entries(block)
    elif isinstance(data,dict):
        for e in data.get("transferCommands",[]):
            yield e

def compare(events, mapping, rd, captured, dump):
    transfers={}
    for e in transfer_entries(rd):
        k=(e["queueSubmitIndex"],e["beginCommandBufferIndex"],e["cmdIndex"])
        require(k not in transfers,"duplicate replay transfer")
        transfers[k]=e
    results=[]
    captured=Path(captured); dump=Path(dump)
    for marker,item in sorted(mapping.items()):
        e=item["snapshot"]
        k=(item["submit_index"],item["begin"],item["command"])
        require(k in transfers,"missing replay transfer "+marker)
        regions=transfers[k]["parameters"]["regions"]
        require(len(regions)==1,"unexpected replay region count")
        region=regions[0]
        sub=region["imageSubresource"];extent=region["imageExtent"]
        require([extent["width"],extent["height"]]==e["extent"] and extent["depth"]==1 and
                sub["mipLevel"]==e["mip"] and sub["baseArrayLayer"]==e["layer"] and sub["layerCount"]==1,
                "replay subresource mismatch")
        # GFXR inserts the buffer filename into the region entry. Reject
        # ambiguous metadata rather than searching by a guessed filename.
        files=[]
        def visit(x):
            if isinstance(x,dict):
                for key,value in x.items():
                    if key=="file" and isinstance(value,str) and value.endswith(".bin"):
                        files.append(value)
                    else: visit(value)
            elif isinstance(x,list):
                for v in x: visit(v)
        visit(region)
        require(len(files)==1,"missing/ambiguous replay buffer filename")
        name=files[0]
        require(Path(name).name==name and Path(e["file"]).name==e["file"],"unsafe file path")
        a=(captured/e["file"]).read_bytes()
        b=(dump/name).read_bytes()
        require(len(a)==len(b)==e["expected_bytes"]==e["captured_bytes"],"byte size mismatch "+marker)
        stride=e["texel_bytes"]
        require(stride>0 and len(a)%stride==0,"bad texel size")
        unequal=sum(a[i:i+stride]!=b[i:i+stride] for i in range(0,len(a),stride)) if a!=b else 0
        results.append({"marker":marker,"role":e["role"],"queue":e["queue"],
                        "frame":e["frame"],"texels":len(a)//stride,"different_texels":unequal,
                        "android_sha256":hashlib.sha256(a).hexdigest(),"reference_sha256":hashlib.sha256(b).hexdigest()})
    # Full native differential only. A common guest translation defect can
    # survive both backends. A boundary mismatch may be irrelevant to the
    # dark scene. These results are NOT P8 PASS/FAIL or causal pruning.
    groups=[e for e in events if e.get("kind")=="consumer"]
    require(groups and not any(e.get("kind")=="missing_consumer" for e in events),"missing known consumer")
    for g in groups:
        snapshots=g.get("snapshots",[])
        require(len(snapshots)==5 and all(s.get("valid") and s["marker"] in mapping for s in snapshots),
                "incomplete five-image boundary")
        require(sorted(s["role"] for s in snapshots)==["depth"]+["gbuffer"]*4,"wrong boundary roles")
        colors=[s for s in snapshots if s["role"]=="gbuffer"]
        require(len({s["layer"] for s in colors})==4 and len({(s["owner"],s["native"],s["version"],s["mip"]) for s in colors})==1,
                "inconsistent material array")
    require({g["queue"] for g in groups}=={0,8,33},"known queue coverage not complete")
    finals=[v["snapshot"] for v in mapping.values() if v["snapshot"]["role"]=="compositor"]
    require(finals,"missing final compositor")
    flips=[e for e in events if e.get("kind")=="flip"]
    def ident(x):
        return tuple(x.get(k) for k in ("owner","native","version","format","width","height"))
    linked={f["marker"] for f in finals if any(ident(f)==ident(g["image"]) and 0<=g["frame"]-f["frame"]<=1 for g in flips)}
    require(linked,"no exact compositor-to-flip identity")
    boundary=[r for r in results if r["role"] in ("gbuffer","depth")]
    changed=any(r["different_texels"] for r in boundary)
    final_changed=any(r["different_texels"] for r in results if r["marker"] in linked)
    if not final_changed: result="NO_FINAL_DIFFERENTIAL"
    elif changed: result="NATIVE_BOUNDARY_DIFF_CANDIDATE"
    else: result="NATIVE_POST_BOUNDARY_DIFF_CANDIDATE"
    return {"p8":"BLOCKED_GUEST_REFERENCE","native_result":result,
            "scope":"Known deferred consumers; same translated Vulkan stream; no guest prefix certificate or causal substitution.",
            "linked_final_markers":sorted(linked),"snapshots":results}

def selftest():
    with tempfile.TemporaryDirectory() as d:
        p=Path(d)
        events=[]
        commands=[{"index":1,"function":{"name":"vkBeginCommandBuffer","args":{"commandBuffer":4}}}]
        rd=[];index=2
        for queue in (0,8,33):
            g=[]
            for layer in range(5):
                role="gbuffer" if layer<4 else "depth"
                n=len(events)+1
                snap={"kind":"snapshot","marker":"P8_FULL_"+str(n),"file":"P8_FULL_"+str(n)+".bin",
                      "role":role,"queue":queue,"frame":400,"valid":True,"extent":[1,1],
                      "mip":0,"layer":layer if layer<4 else 0,"texel_bytes":4,
                      "expected_bytes":4,"captured_bytes":4,"owner":1 if layer<4 else 2,
                      "native":1 if layer<4 else 2,"version":8,"width":1,"height":1,"format":98}
                events.append(snap);g.append(snap)
            events.append({"kind":"consumer","queue":queue,"snapshots":g})
        final=dict(events[0],marker="P8_FULL_FINAL",file="P8_FULL_FINAL.bin",role="compositor",owner=3,native=3)
        events+=[final,{"kind":"flip","frame":401,"image":final}]
        for e in (e for e in events if e["kind"]=="snapshot"):
            (p/e["file"]).write_bytes(b"abcd")
            (p/("r-"+e["file"])).write_bytes(b"abcd")
            sub={"mipLevel":e["mip"],"baseArrayLayer":e["layer"],"layerCount":1}
            extent={"width":1,"height":1,"depth":1}
            region={"imageSubresource":sub,"imageExtent":extent}
            commands+=[{"index":index,"function":{"name":"vkCmdInsertDebugUtilsLabelEXT","args":{"commandBuffer":4,"pLabelInfo":{"pLabelName":e["marker"]}}}},
                       {"index":index+1,"function":{"name":"vkCmdCopyImageToBuffer","args":{"commandBuffer":4,"pRegions":[region]}}}]
            rd.append({"cmdIndex":index+1,"beginCommandBufferIndex":1,"queueSubmitIndex":1000,
                       "parameters":{"regions":[dict(region,file="r-"+e["file"])]}})
            index+=2
        commands.append({"index":1000,"function":{"name":"vkQueueSubmit","args":{"pSubmits":[{"pCommandBuffers":[4]}]}}})
        plan,mapping=native_plan(events,commands)
        assert len(plan["Transfer"][0])==16
        def run(): return compare(events,mapping,[{"transferCommands":rd}],p,p)
        assert run()["native_result"]=="NO_FINAL_DIFFERENTIAL"
        (p/("r-"+final["file"])).write_bytes(b"zzzz")
        assert run()["native_result"]=="NATIVE_POST_BOUNDARY_DIFF_CANDIDATE"
        (p/("r-"+events[0]["file"])).write_bytes(b"zzzz")
        assert run()["native_result"]=="NATIVE_BOUNDARY_DIFF_CANDIDATE"
        assert run()["p8"]=="BLOCKED_GUEST_REFERENCE"
        try:
            native_plan(events,commands[:-1])
        except ValueError:
            pass
        else:
            raise AssertionError("missing submit accepted")
        (p/("r-"+events[0]["file"])).write_bytes(b"z")
        try: run()
        except ValueError: pass
        else: raise AssertionError("truncated replay accepted")
    print("P8 native replay mapping, equality, both differential candidates, missing and truncated evidence: PASS")

def main():
    p=argparse.ArgumentParser()
    sub=p.add_subparsers(dest="cmd",required=True)
    sub.add_parser("selftest")
    a=sub.add_parser("extract");a.add_argument("zip");a.add_argument("directory")
    a=sub.add_parser("plan");a.add_argument("capture");a.add_argument("jsonl");a.add_argument("out")
    a=sub.add_parser("compare");a.add_argument("capture");a.add_argument("mapping");a.add_argument("rd");a.add_argument("dumps")
    args=p.parse_args()
    try:
        if args.cmd=="selftest": selftest();return
        if args.cmd=="extract": print(extract_capture(args.zip,args.directory));return
        events=read_jsonl(Path(args.capture)/"observations.jsonl")
        if args.cmd=="plan":
            plan,mapping=native_plan(events,read_jsonl(args.jsonl))
            Path(args.out).write_text(json.dumps(plan,indent=2))
            Path(args.out+".mapping.json").write_text(json.dumps(mapping,indent=2))
        else:
            print(json.dumps(compare(events,json.loads(Path(args.mapping).read_text()),
                json.loads(Path(args.rd).read_text()),args.capture,args.dumps),indent=2))
    except (ValueError,KeyError,OSError,zipfile.BadZipFile) as e:
        print(json.dumps({"p8":"BLOCKED","reason":str(e)}))
        raise SystemExit(2)

if __name__=="__main__":
    main()

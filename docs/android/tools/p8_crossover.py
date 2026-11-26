#!/usr/bin/env python3
"""Executable P8 causal crossover on the same captured native command stream.

This program actually writes a replay intervention plan, executes the patched
Vulkan replay three times (control, sham, substitution), verifies every
intervened readback and evaluates the flip-linked final output. It does NOT
pretend that native replay alone is an independent guest-ISA reference.

Reference byte files must be produced separately by an independently
executed reference. No expected luminance values or hand-entered PASS flags
are used. The output reports the causal experiment, not a global P8 proof
without closure of the guest prefix.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from p8_native_replay import read_jsonl, sha, require, native_plan, transfer_entries

def identity(s):
    return tuple(s.get(k) for k in
                 ("owner","object","native","version","write_order","format","extent","mip","layer","aspect"))

def file_path(root, name):
    require(isinstance(name,str) and Path(name).name==name,"unsafe payload name")
    p=Path(root)/name
    require(p.is_file() and not p.is_symlink(),"missing payload "+name)
    return p

def transfer_files(mapping, metadata, folder):
    transfers={}
    for e in transfer_entries(metadata):
        k=(e["queueSubmitIndex"],e["beginCommandBufferIndex"],e["cmdIndex"])
        require(k not in transfers,"duplicate transfer")
        transfers[k]=e
    result={}
    def visit(v):
        if isinstance(v,dict):
            for key,x in v.items():
                if key=="file" and isinstance(x,str) and x.endswith(".bin"): yield x
                else: yield from visit(x)
        elif isinstance(v,list):
            for x in v: yield from visit(x)
    for marker,item in mapping.items():
        s=item["snapshot"]
        key=(item["submit_index"],item["begin"],item["command"])
        require(key in transfers,"replay missing "+marker)
        regions=transfers[key]["parameters"]["regions"]
        require(len(regions)==1,"ambiguous region "+marker)
        r=regions[0];sub=r["imageSubresource"];ext=r["imageExtent"]
        require([ext["width"],ext["height"]]==s["extent"] and ext["depth"]==1
                and sub["mipLevel"]==s["mip"] and sub["baseArrayLayer"]==s["layer"]
                and sub["layerCount"]==1,"replay region changed "+marker)
        names=list(visit(r));require(len(names)==1,"ambiguous transfer file "+marker)
        p=file_path(folder,names[0])
        require(p.stat().st_size==s["expected_bytes"],"truncated replay "+marker)
        result[marker]=p
    return result

def command_rows(events, commands):
    plan,mapping=native_plan(events,commands)
    indexed={e["index"]:e.get("function",{}) for e in commands if "index" in e}
    for marker,item in mapping.items():
        s=item["snapshot"];args=indexed[item["command"]]["args"]
        src=args.get("srcImage"); require(type(src)==int and src>0,"no native image id "+marker)
        r=args["pRegions"][0]
        require("aspectMask" in r["imageSubresource"],"missing captured aspect "+marker)
        a=r["imageSubresource"]["aspectMask"]
        if isinstance(a,str):
            a={"VK_IMAGE_ASPECT_COLOR_BIT":1,"VK_IMAGE_ASPECT_DEPTH_BIT":2,
               "VK_IMAGE_ASPECT_STENCIL_BIT":4}.get(a)
        require(a==s["aspect"],"capture aspect mismatch "+marker)
        require(r.get("imageOffset")=={"x":0,"y":0,"z":0},"not a full image copy "+marker)
        item["image"]=src
    verify_use_markers(events,commands,mapping)
    return plan,mapping

def reference_name(marker):
    require(isinstance(marker,str) and re.fullmatch(r"P8_FULL_[A-Za-z0-9_]+",marker),
            "invalid reference marker")
    return marker+".bin"

def verify_use_markers(events,commands,mapping):
    labels={}
    for entry in commands:
        f=entry.get("function") or {}
        if f.get("name")!="vkCmdInsertDebugUtilsLabelEXT":continue
        label=((f.get("args") or {}).get("pLabelInfo") or {}).get("pLabelName")
        if isinstance(label,str) and label.startswith("P8_USE_"):
            require(label not in labels,"duplicate replay use marker "+label)
            labels[label]=entry["index"]
    for use in (e for e in events if e.get("kind")=="cut_use"):
        marker=use.get("marker")
        require(marker in labels,"consumer marker absent in native trace")
        for stage in use.get("stages",[]):
            for b in stage.get("images",[]):
                for snap in b.get("snapshots",[]):
                    m=snap["marker"]
                    require(m in mapping and mapping[m]["command"]<labels[marker]
                            and mapping[m]["submit_index"]<labels[marker],
                            "snapshot transfer not completed before actual use "+m)

def cut_and_final(events,mapping):
    # Keep every captured use's version, not just the first 0/8/33 queue.
    # Reused observations cannot establish a later replay read's state.
    use_count=0;cut=set();used_transfers=set()
    uses=[e for e in events if e.get("kind")=="cut_use"]
    require(uses,"no actual cut-use census")
    for use in uses:
        require(use.get("marker_recorded"),"unrecorded cut use")
        for st in use.get("stages",[]):
            require(st.get("valid"),"missing stage")
            for b in st.get("images",[]):
                if not b.get("cut_candidate"):continue
                require(b.get("access_known"),"unknown cut access")
                if not b.get("read"):continue
                use_count+=1
                require(not b.get("reason") and b.get("snapshots"),"incomplete cut read")
                for s in b["snapshots"]:
                    m=s["marker"]; require(m in mapping,"missing cut snapshot")
                    require(identity(s)==identity(mapping[m]["snapshot"]),"wrong cut identity")
                    # Retaining a copy from an earlier command is not an
                    # observation at this consumer. Refuse instead of assuming.
                    require(s.get("submit")==use.get("submit"),"cut read reused from another submit")
                    require(m not in used_transfers,"transfer reused for multiple cut reads")
                    used_transfers.add(m);cut.add(m)
    require(cut and use_count,"empty cut")
    flips=[x for x in events if x.get("kind")=="flip"]
    def fid(s):return tuple(s.get(k) for k in ("owner","native","version","format","width","height"))
    finals=set()
    for m,item in mapping.items():
        s=item["snapshot"]
        if s["role"]=="compositor" and any(fid(s)==fid(f["image"]) and
             0<=f["frame"]-s["frame"]<=1 for f in flips): finals.add(m)
    require(len(finals)==1,"no unique flip-linked final output")
    require(max(mapping[m]["submit_index"] for m in cut)<max(mapping[m]["submit_index"] for m in finals),
            "cut/final ordering not proved")
    return sorted(cut),next(iter(finals))

def write_plan(out, mapping, payloads):
    # Paths have no whitespace by design, so the native reader can remain
    # dependency-free. Use a private temporary directory for all raw payloads.
    rows=[]
    for m,p in sorted(payloads.items(),key=lambda kv:mapping[kv[0]]["command"]):
        it=mapping[m];s=it["snapshot"];p=Path(p).resolve()
        require(not re.search(r"\s",str(p)),"splice path has whitespace")
        require(p.stat().st_size==s["expected_bytes"],"payload size "+m)
        rows.append((it["command"],it["image"],s["format"],s["aspect"],s["mip"],s["layer"],
                     *s["extent"],s["expected_bytes"],str(p)))
    require(rows,"empty intervention")
    Path(out).write_text("KYTY_P8_SPLICE_V1\n"+"".join(" ".join(map(str,row))+"\n" for row in rows))
    return {(r[0],r[1],r[-2]) for r in rows}

def run_one(replayer, trace, plan, mapping, work, substitutions, extra):
    work.mkdir()
    dump=work/"images";dump.mkdir()
    out=dump/(trace.stem+"_rd.json")
    env=os.environ.copy()
    env.pop("KYTYP8_SPLICE_PLAN",None);env.pop("KYTYP8_SPLICE_AUDIT",None)
    if substitutions:
        audit=work/"splice.log";sp=work/"splice.txt"
        wanted=write_plan(sp,mapping,substitutions)
        env.update(KYTYP8_SPLICE_PLAN=str(sp),KYTYP8_SPLICE_AUDIT=str(audit))
    # GFXR names its metadata after the input capture. Use its documented
    # options directly; do not require a nonexistent metadata CLI flag.
    require(not any(a.startswith("--dump-resources") for a in extra),"duplicate dump option")
    argv=[str(Path(replayer).resolve()),*extra,"--dump-resources",str(plan),
          "--dump-resources-dir",str(dump),str(trace)]
    with (work/"replay.log").open("wb") as log:
        subprocess.run(argv,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    if substitutions:
        got=[tuple(map(int,l.split())) for l in audit.read_text().splitlines()]
        require(len(got)==len(wanted) and set(got)==wanted,"not all interventions executed exactly once")
    require(out.is_file(),"GFXR did not emit the expected transfer metadata")
    return transfer_files(mapping,json.loads(out.read_text()),dump)

def assess(mapping, cut, final, captured, reference, control, sham, crossed):
    def matches(a,b,m):return sha(a[m])==sha(b[m])
    require(all(matches(captured,control,m) for m in [*cut,final]),"baseline not reproduced")
    require(all(matches(control,sham,m) for m in [*cut,final]),"sham intervention changes execution")
    require(all(matches(reference,crossed,m) for m in cut),"replacement not read back exactly")
    changes=[m for m in cut if not matches(captured,reference,m)]
    require(not matches(captured,reference,final),"reference does not differ at the linked output")
    result="NATIVE_CAUSAL_UNRESOLVED"
    if not changes:
        require(matches(control,crossed,final),"equal cut changes output")
        result="NATIVE_POST_BOUNDARY_DIFFERENTIAL"
    elif matches(reference,crossed,final):
        result="NATIVE_PRE_BOUNDARY_SUFFICIENT"
    elif not matches(control,crossed,final):
        result="NATIVE_PRE_BOUNDARY_CONTRIBUTION"
    return {"native_causal_result":result,"changed_cut_markers":changes,"final":final,
            "captured_final_sha256":sha(captured[final]),"reference_final_sha256":sha(reference[final]),
            "crossed_final_sha256":sha(crossed[final]),"p8":"BLOCKED_GUEST_PREFIX",
            "scope":"actual native counterfactual; independent guest prefix and reference authority are separate"}

def main():
    p=argparse.ArgumentParser()
    p.add_argument("capture",type=Path);p.add_argument("commands",type=Path)
    p.add_argument("reference",type=Path,help="exact raw reference files named by unique transfer marker plus .bin")
    p.add_argument("replayer",type=Path);p.add_argument("work",type=Path)
    p.add_argument("replay_args",nargs=argparse.REMAINDER)
    a=p.parse_args()
    try:
        events=read_jsonl(a.capture/"observations.jsonl")
        plan,mapping=command_rows(events,read_jsonl(a.commands))
        cut,final=cut_and_final(events,mapping)
        captured={m:file_path(a.capture,mapping[m]["snapshot"]["file"]) for m in mapping}
        # Android payloads may be deduplicated. Reference payloads are always
        # named by the unique transfer marker, not by the Android file alias.
        reference={m:file_path(a.reference,reference_name(m)) for m in [*cut,final]}
        for m in reference:
            require(reference[m].stat().st_size==captured[m].stat().st_size,"reference length "+m)
        a.work=a.work.resolve();a.work.mkdir(exist_ok=False)
        plan_path=a.work/"dump-plan.json";plan_path.write_text(json.dumps(plan))
        traces=list(a.capture.glob("*.gfxr"));require(len(traces)==1,"ambiguous trace")
        args=a.replay_args
        if args and args[0]=="--":args=args[1:]
        def run(name,swap):return run_one(a.replayer,traces[0].resolve(),plan_path,mapping,a.work/name,swap,args)
        control=run("control",{})
        sham=run("sham",{m:captured[m] for m in cut})
        crossed=run("crossed",{m:reference[m] for m in cut})
        print(json.dumps(assess(mapping,cut,final,captured,reference,control,sham,crossed),indent=2))
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as e:
        print(json.dumps({"p8":"BLOCKED","reason":str(e)}))
        raise SystemExit(2)
if __name__=="__main__":main()

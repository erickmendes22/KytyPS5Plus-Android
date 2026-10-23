#!/usr/bin/env python3
"""tools/gputhread_split.py <_perf_profile.txt.gz> <unstripped libmain.so>: GPU-thread wall-stack samples bucketed by stage
(SRT walk, program lookup, binding prep, record, PM4, waits, readback). See research/gputhread_pipeline.md."""
import collections, importlib.util, os, re, sys
spec = importlib.util.spec_from_file_location("profsym", os.environ.get("KYTY_PROFSYM", os.path.join(os.path.dirname(os.path.abspath(__file__)), "profsym.py")))
ps = importlib.util.module_from_spec(spec); spec.loader.exec_module(ps)
path, libmain = sys.argv[1], sys.argv[2]
libs = {"libmain.so": libmain, "libc.so": os.environ.get("KYTY_LIBC", "libc.so")}  # KYTY_LIBC: the tablet's libc.so pulled with adb
lines = ps.read(path); cur=None; stacks=[]
for line in lines:
    if line.startswith("## "): cur=line[3:].split()[0]; continue
    if cur!="wall-stacks": continue
    m=ps.ENTRY.match(line)
    if not m: continue
    n,rest=int(m.group(1)),m.group(2)
    th,rest=rest.split(": ",1) if ": " in rest else ("",rest)
    stacks.append((n,th,[f.strip() for f in rest.split("<-")]))
want=collections.defaultdict(set)
for _,_,fr in stacks:
    for f in fr:
        for lib,off in ps.LIBOFF.findall(f):
            if lib in libs: want[lib].add(int(off,16))
names={lib:ps.symbolize(libs[lib],sorted(o)) for lib,o in want.items()}
def nm(f):
    m=ps.LIBOFF.search(f)
    if m and m.group(1) in names: return m.group(1).split('.')[0]+":"+names[m.group(1)][int(m.group(2),16)]
    return f
CATS=[("readback (GPU->CPU sync)", r"ReadBackInPlace|DownloadBufferMemory|BufferCache::ReadMemory|ReadBack"),
      ("SRT walk/materialize", r"SrtWalker|MaterializeResources|SrtReadCapture|ReadGuestForSrt|ResourceMaterial|Specializ"),
      ("program lookup", r"FindPermutation|ProgramCache|GetGraphicsPrograms|GetComputeProgram|RefreshShaders|GetComputePipeline|GetGraphicsPipeline|PipelineCache"),
      ("submit/flush", r"QueueSubmit|::Submit|CommandScheduler::Flush|EndCommandBuffer|Present|Flip"),
      ("wait (sync/fence/idle)", r"SendCommandSync|Wait|Fence|Idle|Sleep|ThreadRun|CommandScheduler::Active|MadeProgress"),
      ("binding prep", r"PrepareBindings|ResolveTexture|PreparedVertexBuffers|PrepareDrawRenderState|ResolveRender|TextureCache|BufferCache|FindImage|ImageInfo|RebindImages|RebindBuffers|FindBuffers|CommitBindings|Descriptor"),
      ("vulkan record", r"ExecutePreparedDraw|vk::|Vk|CmdDraw|BeginRendering|EndRendering|Barrier|bindPipeline|pushConstants|libvulkan"),
      ("PM4 parse/dispatch", r"ProcessPm4|Pm4Execution|CommandProcessor|Pm4")]
tot=sum(n for n,_,_ in stacks); bucket=collections.Counter(); detail=collections.defaultdict(collections.Counter)
for n,th,fr in stacks:
    syms=[nm(f) for f in fr]
    lab="other"
    for s in syms:
        for c,pat in CATS:
            if re.search(pat,s): lab=c; break
        if lab!="other": break
    bucket[lab]+=n
    detail[lab][syms[0][:90]]+=n
print(f"# {os.path.basename(os.path.dirname(path))}: {tot} wall samples (1 ms) on {collections.Counter(t for _,t,_ in stacks).most_common(2)}")
for c,n in bucket.most_common():
    print(f"{n:6d} {100*n/tot:5.1f}%  {c}")
    for k,v in detail[c].most_common(4): print(f"           {v:5d}  {k}")

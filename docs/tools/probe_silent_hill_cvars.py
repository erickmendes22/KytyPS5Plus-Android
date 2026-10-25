"""Reproduce isolated x86 CVar constructor/setter checks for the supplied SELF.

Requires unicorn==2.1.4. Does not boot a game or call native guest code.
Allocator, help/name/string formatting, parser and manager insertion are stubs;
constructor instructions, priority decisions and setting writes are guest code.
"""
from pathlib import Path
import argparse
import hashlib,json,struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("self",type=Path)
parser.add_argument("--plan",type=Path,default=Path(__file__).resolve().parents[1]/"SILENT_HILL_PATCH_PLAN.json")
args=parser.parse_args()
raw=args.self.read_bytes()
plan=json.loads(args.plan.read_text())
assert hashlib.sha256(raw).hexdigest()==plan['self_sha256'], "SELF build mismatch"
count=struct.unpack_from('<H',raw,24)[0]
elf=32+32*count
phoff=struct.unpack_from('<Q',raw,elf+32)[0]
segs=[]
for index in range(count):
 kind,offset,size,_=struct.unpack_from('<QQQQ',raw,32+32*index)
 if kind & 0x800:
  ph=struct.unpack_from('<IIQQQQQQ',raw,elf+phoff+56*((kind>>20)&4095))
  segs.append((ph[3],offset,size))
BASE=0x900000000;OBJ=0x70000000;CTX=0x70001000;VT=0x70002000;STACK=0x71000000;RETURN=0x72000000
u=Uc(UC_ARCH_X86,UC_MODE_64);u.ctl_set_cpu_model(UC_CPU_X86_HASWELL)
for va,off,size in segs:
 if va==0 and size!=0x57554cc:continue
 data=bytearray(raw[off:off+size])
 if va==0:
  for site in plan['registrations']:
   a=int(site['rva'],16);old=bytes.fromhex(site['off']);assert data[a:a+len(old)]==old;data[a:a+len(old)]=bytes.fromhex(site['on'])
 if va==0x7b10000:
  fog=plan['fog_storage'];at=int(fog['rva'],16)-va
  assert data[at:at+4]==bytes.fromhex(fog['off'])
  data[at:at+4]=bytes.fromhex(fog['on'])
 addr=BASE+va;start=addr&~4095;end=(addr+size+4095)&~4095
 # Adjacent segments may share a page; these real segments do not overlap except metadata.
 try:u.mem_map(start,end-start)
 except Exception as e:print(hex(start),hex(end),e);raise
 u.mem_write(addr,bytes(data))
for addr,size in [(OBJ,0x10000),(STACK,0x10000),(RETURN,0x1000)]:u.mem_map(addr,size)
def wq(a,v):u.mem_write(a,struct.pack('<Q',v))
wq(CTX,VT);wq(VT+8,BASE+0x1217540);wq(VT+0x10,BASE+0x12175f0);wq(VT+0x30,BASE+0x1217df0)
# GetAsVariable int/float/ref share the same identity return.
for a in [0x74b7fe0,0x74b80b0,0x74b8320]:wq(BASE+a+0x30,BASE+0x1220f00)
# Actual constructors write their flags and both value copies; only allocator,
# help-string ownership, and manager insertion are stubbed in this isolated probe.
stubs={BASE+0xfbb010:'alloc',BASE+0x1220cd0:'help',BASE+0x12166c0:'register'}
stop=None
def hook(uc,address,size,data):
 if address==stop:uc.emu_stop();return
 if address in stubs:
  kind=stubs[address]
  if kind=='alloc':uc.reg_write(UC_X86_REG_RAX,OBJ)
  if kind=='string':
   dest=uc.reg_read(UC_X86_REG_RDI);uc.mem_write(dest,b'\0'*16);uc.reg_write(UC_X86_REG_RAX,dest)
  if kind=='parse':uc.reg_write(UC_X86_REG_RAX,3)
  if kind=='parsefloat':uc.reg_write(UC_X86_REG_XMM0,0x40400000)
  if kind=='register':
   assert uc.reg_read(UC_X86_REG_RDX)==OBJ
   uc.reg_write(UC_X86_REG_RAX,OBJ)
  sp=uc.reg_read(UC_X86_REG_RSP);ret=struct.unpack('<Q',uc.mem_read(sp,8))[0]
  uc.reg_write(UC_X86_REG_RSP,sp+8);uc.reg_write(UC_X86_REG_RIP,ret)
u.hook_add(UC_HOOK_CODE,hook)
for site in plan['registrations']:
 name=site['effect'];a=int(site['rva'],16);stop=BASE+a+len(bytes.fromhex(site['on']))
 u.mem_write(OBJ,b'\0'*256)
 sp=STACK+0x8000;wq(sp,RETURN)
 u.reg_write(UC_X86_REG_RSP,sp);u.reg_write(UC_X86_REG_RDI,CTX);u.reg_write(UC_X86_REG_RAX,VT)
 u.reg_write(UC_X86_REG_RDX,0x12345678);u.reg_write(UC_X86_REG_XMM0,0)
 try:u.emu_start(BASE+a,stop,count=1000)
 except Exception:
  print('FAILED',name,hex(u.reg_read(UC_X86_REG_RIP)));raise
 assert u.reg_read(UC_X86_REG_RIP)==stop,name
 flags=struct.unpack('<I',u.mem_read(OBJ+0x18,4))[0];assert flags>>24==9,(name,hex(flags))
 if name=='Resolution':
  values=struct.unpack('<ff',u.mem_read(OBJ+0x44,8));assert values==(50.,50.),values
 elif name=='Fog':
  pointer=struct.unpack('<Q',u.mem_read(OBJ+0x48,8))[0];assert pointer==BASE+0x7b27f4c
  values=(struct.unpack('<I',u.mem_read(pointer,4))[0],struct.unpack('<I',u.mem_read(OBJ+0x50,4))[0]);assert values==(0,0),values
 else:
  values=struct.unpack('<ii',u.mem_read(OBJ+0x44,8));assert values==(0,0),values
 print(name,'flags',hex(flags),'game/render values',values)
# Run the actual integer setter and its actual priority predicate. Only diagnostic
# name/string formatting is stubbed; the priority branch and value store are real.
stubs.update({BASE+0x1218850:'string',BASE+0x11debe0:'string',BASE+0x1215d20:'string',BASE+0x574d8e0:'parse'})
wq(BASE+0x7b05228,OBJ+0x300);wq(OBJ+0x300,0x123456789abcdef0)
u.mem_map(BASE+0x7bec000,0x1000)
wq(BASE+0x7bec760,CTX)
wq(BASE+0x74b7fe0+0xa8,BASE+0x1215d20)
u.mem_write(OBJ+0x100,'3\0'.encode('utf-16le'))
for source in range(10):
 wq(OBJ,BASE+0x74b7fe0)
 u.mem_write(OBJ+0x18,struct.pack('<I',0x09000060))
 u.mem_write(OBJ+0x44,b'\0'*8)
 sp=STACK+0x8000;wq(sp,RETURN)
 u.reg_write(UC_X86_REG_RSP,sp);u.reg_write(UC_X86_REG_RDI,OBJ)
 u.reg_write(UC_X86_REG_RSI,OBJ+0x100);u.reg_write(UC_X86_REG_RDX,source<<24)
 stop=RETURN if source<9 else BASE+0x1227751
 try:u.emu_start(BASE+0x1227720,stop,count=3000)
 except Exception:
  print('SET FAILED',source,hex(u.reg_read(UC_X86_REG_RIP)));raise
 assert u.reg_read(UC_X86_REG_RIP)==stop
 values=struct.unpack('<ii',u.mem_read(OBJ+0x44,8))
 assert values==(0,0) if source<9 else values[0]==3,(source,values)
 print('Set priority',source,'rejected' if source<9 else 'accepted','values',values)
stubs[BASE+0x574de50]='parsefloat'
wq(BASE+0x74b80b0+0xa8,BASE+0x1215d20)
for source in range(10):
 wq(OBJ,BASE+0x74b80b0)
 u.mem_write(OBJ+0x18,struct.pack('<I',0x09000040))
 u.mem_write(OBJ+0x44,struct.pack('<ff',50.,50.))
 sp=STACK+0x8000;wq(sp,RETURN)
 u.reg_write(UC_X86_REG_RSP,sp);u.reg_write(UC_X86_REG_RDI,OBJ)
 u.reg_write(UC_X86_REG_RSI,OBJ+0x100);u.reg_write(UC_X86_REG_RDX,source<<24)
 stop=RETURN if source<9 else BASE+0x12279fe
 u.emu_start(BASE+0x12279d0,stop,count=3000)
 assert u.reg_read(UC_X86_REG_RIP)==stop
 values=struct.unpack('<ff',u.mem_read(OBJ+0x44,8))
 assert values==(50.,50.) if source<9 else values[0]==3.,(source,values)
 print('Float Set priority',source,'rejected' if source<9 else 'accepted','values',values)

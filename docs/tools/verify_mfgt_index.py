"""Private-input reproduction of MFGT index compatibility checks.
Requires capstone. Prints checks only; does not export state or decoded game data.
"""
from pathlib import Path
import capstone,struct,json
parser=__import__('argparse').ArgumentParser(description='Validate the supplied MFGT SELF/index pair without booting or modifying it.')
parser.add_argument('self',type=Path)
parser.add_argument('index',type=Path)
args=parser.parse_args()
raw=args.self.read_bytes()
assert __import__('hashlib').sha256(raw).hexdigest()=='7e03d8f703f1cd7dc2cc9e6c55318450a6701725a6f66a183a6489e519bb7118', 'SELF mismatch'
source=args.index.read_bytes()
assert __import__('hashlib').sha256(source).hexdigest()=='211323b5e5c63a5957450520aacc7cf239edaa89578e2c528051c7895d6de44f', 'index mismatch'
count=struct.unpack_from('<H',raw,24)[0];elf=32+32*count;phoff=struct.unpack_from('<Q',raw,elf+32)[0];segs=[]
for i in range(count):
 kind,offset,size,_=struct.unpack_from('<QQQQ',raw,32+32*i)
 if kind & 0x800:
  ph=struct.unpack_from('<IIQQQQQQ',raw,elf+phoff+56*((kind>>20)&4095));segs.append((ph[3],offset,size))
code=raw[next(o for v,o,n in segs if v==0 and n==0x3e4548c):][:0x3e4548c]
cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def get(a,n):
 for v,o,z in segs:
  if v<=a<v+z:return raw[o+a-v:o+a-v+n]
def engine(state):
 regs={f'xmm{i}':struct.pack('<4I',*state[i*4:i*4+4]) for i in range(4)}
 regs.update({'xmm4':regs['xmm0'],'xmm5':regs['xmm2'],'xmm6':regs['xmm1'],'xmm7':regs['xmm3'],'xmm9':get(0x4707550,16),'xmm10':get(0x4703340,16),'xmm11':get(0x4702e90,16)})
 instructions=list(cs.disasm(code[0x3dea453:0x3dea504],0x3dea453))
 for _ in range(10):
  for ins in instructions:
   if ins.mnemonic=='dec':continue
   dest,a,b=ins.op_str.split(', ');left=regs[a]
   if ins.mnemonic=='vpshufb':
    mask=regs[b];out=bytes(0 if x&128 else left[x&15] for x in mask)
   elif ins.mnemonic=='vpshufd':
    imm=int(b,0);words=struct.unpack('<4I',left);out=struct.pack('<4I',*[words[(imm>>(2*i))&3] for i in range(4)])
   elif ins.mnemonic in ['vpsrld','vpslld']:
    shift=int(b,0);words=struct.unpack('<4I',left);out=struct.pack('<4I',*[(x>>shift) if ins.mnemonic=='vpsrld' else (x<<shift)&0xffffffff for x in words])
   else:
    right=regs[b]
    if ins.mnemonic=='vpaddd':out=struct.pack('<4I',*[(x+y)&0xffffffff for x,y in zip(struct.unpack('<4I',left),struct.unpack('<4I',right))])
    elif ins.mnemonic=='vpxor':out=bytes(x^y for x,y in zip(left,right))
    elif ins.mnemonic=='vpor':out=bytes(x|y for x,y in zip(left,right))
    else:raise ValueError(ins.mnemonic)
   regs[dest]=out
 x=[]
 for key in ['xmm4','xmm6','xmm5','xmm7']:x+=list(struct.unpack('<4I',regs[key]))
 return [(a+b)&0xffffffff for a,b in zip(x,state)]
def standard(state):
 x=state.copy()
 def rol(v,n):return ((v<<n)|(v>>(32-n)))&0xffffffff
 def qr(a,b,c,d):
  x[a]=(x[a]+x[b])&0xffffffff;x[d]=rol(x[d]^x[a],16)
  x[c]=(x[c]+x[d])&0xffffffff;x[b]=rol(x[b]^x[c],12)
  x[a]=(x[a]+x[b])&0xffffffff;x[d]=rol(x[d]^x[a],8)
  x[c]=(x[c]+x[d])&0xffffffff;x[b]=rol(x[b]^x[c],7)
 for _ in range(10):
  for q in [(0,4,8,12),(1,5,9,13),(2,6,10,14),(3,7,11,15),(0,5,10,15),(1,6,11,12),(2,7,8,13),(3,4,9,14)]:qr(*q)
 return [(a+b)&0xffffffff for a,b in zip(x,state)]
if __name__=='__main__':
 for seed in range(8):
  state=[(i+seed*0x1234567)&0xffffffff for i in range(16)];state[12]=0
  assert engine(state)==standard(state),(engine(state),standard(state))
 print('Manual vector-semantics reference matches standard ChaCha20 on 8 independent states')

 state=list(struct.unpack('<16I',get(0x4afa250,64)))
 decoded=bytearray(source)
 for offset in range(0,len(source),64):
  state[12]=offset//64
  stream=struct.pack('<16I',*standard(state))
  for j in range(min(64,len(source)-offset)):decoded[offset+j]^=stream[j]
 seed=0x1fd5b434
 for offset in range(0,0x800,4):
  word=struct.unpack_from('<I',decoded,offset)[0];crc=seed
  for _ in range(32):crc=((crc<<1)^(0x04c11db7 if crc&0x80000000 else 0))&0xffffffff
  struct.pack_into('<I',decoded,offset,~(crc^word)&0xffffffff);seed=word
 assert struct.unpack_from('<I',decoded)[0]==0x122bb8f2
 assert struct.unpack_from('<I',decoded,32)[0]==0x11160000
 section_start,section_size=struct.unpack_from('<II',decoded,0x24)
 assert section_start+section_size<=len(decoded)
 section=decoded[section_start:section_start+section_size]
 first_bytes=struct.unpack_from('<Q',section,16)[0]
 assert first_bytes+36<=len(section)
 count=struct.unpack_from('<I',section,first_bytes+24)[0]
 words=struct.unpack_from('<Q',section,first_bytes+28)[0]
 assert first_bytes+36+4*words==len(section)
 bits=struct.unpack_from('<'+str(words)+'I',section,first_bytes+36)
 assert sum(word.bit_count() for word in bits)==count
 entry_start,entry_bytes=struct.unpack_from('<II',decoded,0x2c)
 assert entry_start+entry_bytes<=len(decoded) and entry_bytes%24==0
 assert entry_bytes//24==count==7857
 digest=14695981039346656037
 for byte in decoded:digest=((digest^byte)*1099511628211)&0xffffffffffffffff
 assert digest==0x18853ced4a25d0fa
 print(f'Valid decoded header; {count} catalog/entry records; complete decoded FNV {digest:016x}')

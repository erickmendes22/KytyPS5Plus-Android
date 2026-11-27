"""Schema-4 BRDF slice, original ISA vs identical observed operands.

The captured live-ins are not assumed correct. Reinject observed destinations
only in the OFFLINE reference, so each next operation has native inputs.
No changes to in-game light or material values.
"""
import numpy as np
LIVE=(12,13,14,18,19,20,21,22,26,27,28,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,48,49,50,51,52,53,54,55,60,61,62)
OPS=tuple((int(p,16),int(r)) for p,r in (x.split(':') for x in '1bb4:0 1bc0:55 1bc8:2 1bcc:54 1bd4:1 1bd8:53 1be0:0 1be4:57 1be8:52 1bec:57 1bf0:52 1bf4:57 1bf8:52 1bfc:56 1c00:54 1c04:52 1c08:53 1c0c:56 1c10:55 1c14:2 1c18:56 1c1c:55 1c20:2 1c24:56 1c2c:55 1c30:1 1c38:2 1c40:52 1c44:0 1c4c:54 1c54:2 1c5c:52 1c64:53 1c6c:52 1c70:48 1c74:53 1c7c:2 1c80:52 1c84:52 1c8c:52 1c90:48 1c94:52 1c9c:1 1ca4:53 1ca8:1 1cb0:53 1cb4:1 1cb8:48 1cbc:48 1cc0:2 1cc4:48 1ccc:2 1cd0:52 1cd8:2 1cdc:48 1ce0:0 1ce4:54 1ce8:52 1cf0:53 1cf8:51 1d00:2 1d04:52 1d08:54 1d0c:48 1d10:50 1d14:54 1d18:53 1d1c:51 1d20:50 1d24:52 1d28:1 1d2c:53 1d34:49 1d38:0 1d3c:51 1d40:52 1d48:0 1d4c:50 1d50:1'.split()))
LIGHTS=224
SCALAR=1+len(LIVE); RESULTS=SCALAR+2; STRIDE=RESULTS+2*len(OPS); EXTRA_WORDS=2*LIGHTS*STRIDE

def extract(raw,pair_words,role):
    from p8_paired_replay import need
    need(role in ('dark','control') and len(raw)==4*(pair_words+EXTRA_WORDS),'BRDF trace layout')
    start=pair_words+(LIGHTS*STRIDE if role=='control' else 0)
    return np.frombuffer(raw,'<u4')[start:start+LIGHTS*STRIDE].reshape(LIGHTS,STRIDE)

def validate(row,record,term):
    from p8_paired_replay import need
    need(len(row)==STRIDE,'BRDF row size')
    if not int(record[30]):
        # EXEC may be restored for the last three adds even without BRDF entry.
        need(not np.any(row[:RESULTS+2*(len(OPS)-3)]),'BRDF data without matching post-cube entry')
        return False
    need(int(record[30])==0x1bb5 and int(row[0])==0x1bb5,'BRDF entry missing')
    need(int(row[SCALAR])==0x1bb9,'BRDF scalar marker missing')
    for j,(pc,dst) in enumerate(OPS):need(int(row[RESULTS+2*j])==pc+1,'BRDF instruction missing '+hex(pc))
    need([int(row[1+LIVE.index(r)]) for r in (49,50,51)]==list(map(int,record[31:34])),'BRDF entry not bound to RGB trace')
    need(list(map(int,row[RESULTS+2*(len(OPS)-3)+1::2]))==list(map(int,term[[2,4,6]])),'BRDF final term not bound to accumulation')
    return True

def same_input(code,runner,mode,lane,sizes,row):
    from v4_q8_pair_replay import need,run_wave,mov,lit,exp,END,dist
    native=int(row[SCALAR+1])
    if native!=0x3faaaaab:return dict(status='DIVERGENCE_CANDIDATE',first=dict(pc='0x1bb8',expected=0x3faaaaab,observed=native))
    v=[0]*(256*64)
    for j,r in enumerate(LIVE):v[r*64+lane]=int(row[1+j])
    p=bytearray()
    for j,(pc,dst) in enumerate(OPS):
        if j==1:p+=code[0x1bb8:0x1bc0]
        p+=code[pc:pc+sizes[pc]]+mov(100+j%4,256+dst)+lit(dst,int(row[RESULTS+2*j+1]))
        if j%4==3 or j+1==len(OPS):
            p+=exp(32+j//4,[100,101,102,103],j+1==len(OPS))
    p+=END
    exports=run_wave(runner,bytes(p),[0]*106,v,[],mode,1<<lane)
    differences=[];max_ulp=0
    for j,(pc,dst) in enumerate(OPS):
        need((32+j//4,lane) in exports,'missing BRDF reference export')
        expected=int(exports[32+j//4,lane][2][j%4]);observed=int(row[RESULTS+2*j+1]);d=dist(expected,observed);max_ulp=max(max_ulp,d)
        if d>8:differences.append(dict(pc=hex(pc),dst=dst,expected=expected,observed=observed,ulp=d))
    return dict(status='DIVERGENCE_CANDIDATE' if differences else 'MATCH_LOCAL',first=differences[0] if differences else None,comparison_count=len(OPS),max_ulp=max_ulp,differences=differences)

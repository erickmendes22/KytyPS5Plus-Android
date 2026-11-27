#!/usr/bin/env python3
"""Versioned logical texture reference for scene replay, not a hardware oracle.

Only decoded, retained subresources are accepted. No inferred missing texel,
placeholder binding, physical tiling, DCC or unverified sampler is certified.
An exact sampler contract can be executed for point/linear, mip0 or explicit
LOD; implicit derivatives are evaluated per complete 2x2 quad when enabled.
"""
import math
import struct
from dataclasses import dataclass
import numpy as np
from p8_bilateral import require
from p8_image_trap import floatbits,floatword


def decode_pixels(raw,fmt,w,h,d=1):
    require(all(type(n) is int and n>0 for n in (w,h,d)), 'invalid texture extent')
    n=w*h*d
    require(n<=32*1024*1024,'texture decode budget exceeded')
    a=np.zeros((n,4),np.float32);a[:,3]=1
    if fmt in (16,37,43):
        c=2 if fmt==16 else 4
        require(len(raw)==n*c,'truncated UNORM image')
        a[:,:c]=np.frombuffer(raw,np.uint8).reshape(n,c).astype(np.float32)/np.float32(255)
    elif fmt in (76,83,97,100,126,130):
        c={76:1,83:2,97:4,100:1,126:1,130:1}[fmt]
        t='<f2' if fmt in (76,83,97) else '<f4'
        require(len(raw)==n*c*np.dtype(t).itemsize,'truncated float image')
        a[:,:c]=np.frombuffer(raw,t).reshape(n,c)
    elif fmt==122: # VK_FORMAT_B10G11R11_UFLOAT_PACK32
        require(len(raw)==n*4,'truncated R11G11B10 image')
        u=np.frombuffer(raw,'<u4')
        for j,(shift,m) in enumerate(((0,6),(11,6),(22,5))):
            q=(u>>shift)&((1<<(m+5))-1);e=(q>>m).astype(np.int32);f=(q&((1<<m)-1)).astype(np.float32)/np.float32(1<<m)
            a[:,j]=np.where(e==0,np.ldexp(f,-14),np.ldexp(1+f,e-15));a[e==31,j]=np.where(f[e==31]==0,np.inf,np.nan)
    elif fmt in (137,138,145,146): # BC3/BC7, UNORM/SRGB
        from PIL import Image
        require(d==1 and len(raw)==((w+3)//4)*((h+3)//4)*16,'truncated compressed image')
        # Pillow's BCN decoder accepts block-compressed RGBA payloads.
        im=Image.frombytes('RGBA',(w,h),raw,'bcn',3 if fmt in (137,138) else 7)
        a=np.asarray(im,dtype=np.float32).reshape(n,4)/np.float32(255)
    else:raise ValueError('unsupported logical VkFormat '+str(fmt))
    if fmt in (43,138,146):
        rgb=a[:,:3];a[:,:3]=np.where(rgb<=np.float32(.04045),rgb/np.float32(12.92),((rgb+np.float32(.055))/np.float32(1.055))**np.float32(2.4))
    require(np.isfinite(a).all(),'nonfinite logical texture')
    return a.reshape(d,h,w,4)


@dataclass(frozen=True)
class Sampler:
    mag:str
    min:str
    mip:str
    address:tuple
    min_lod:float=0.
    max_lod:float=0.
    bias:float=0.
    border:tuple=(0.,0.,0.,0.)
    normalized:bool=True
    implicit:bool=False


@dataclass
class Texture:
    # Map (absolute mip, absolute array layer) -> logical [depth,h,w,4].
    # All entries must refer to the same captured owner/object/native/version.
    levels:dict
    base_mip:int=0
    last_mip:int=0
    base_layer:int=0
    layer_count:int=1
    dim:str='SQ_RSRC_IMG_2D'
    select:tuple=(4,5,6,7)
    extent:tuple=()

    def get(self,m,l):
        require(self.base_mip<=m<=self.last_mip and self.base_layer<=l<self.base_layer+self.layer_count,'subresource outside view')
        require((m,l) in self.levels,'unretained mip/layer '+str((m,l)))
        a=self.levels[m,l]
        require(a.ndim==4 and a.shape[-1]==4 and a.dtype==np.float32 and all(a.shape),'invalid logical subresource')
        if self.extent:
            w,h,d=self.extent;expect=(max(1,d>>m) if self.dim=='SQ_RSRC_IMG_3D' else 1,max(1,h>>m),max(1,w>>m),4)
            require(a.shape==expect,'subresource extent mismatch')
        return a


class VersionedTextures:
    def __init__(self,entries,samplers):
        self.entries=entries;self.samplers=samplers;self.reads=[]

    @staticmethod
    def _index(i,n,mode):
        if mode=='wrap':return i%n
        if mode=='mirror':
            k=i%(2*n);return k if k<n else 2*n-1-k
        if mode=='clamp':return min(n-1,max(0,i))
        if mode=='border':return i if 0<=i<n else None
        raise ValueError('unimplemented address mode '+str(mode))

    def __call__(self,r,desc,sampler,coords):
        require(desc in self.entries,'missing versioned descriptor '+str(desc));t=self.entries[desc]
        require(t.dim==r.dim and len(t.select)==4 and all(x in (0,1,4,5,6,7) for x in t.select),'descriptor dimension/component map')
        s=self.samplers.get(sampler) if r.op!='image_load' else None
        if r.op!='image_load':
            require(s is not None,'unverified sampler '+str(sampler))
            require(s.mag in ('nearest','linear') and s.min in ('nearest','linear') and s.mip in ('none','nearest','linear') and len(s.address)==3,'unsupported sampler contract')
            require(s.normalized or s.mip=='none','unnormalized mip sampling')
        out={};support={};array=t.dim=='SQ_RSRC_IMG_2D_ARRAY';volume=t.dim=='SQ_RSRC_IMG_3D';nd=3 if volume else 2
        for lane,words in coords.items():
            used=set();layer=t.base_layer
            if r.op=='image_load':
                q=[struct.unpack('<i',struct.pack('<I',x))[0] for x in words]
                if array:layer+=q[2]
                a=t.get(t.base_mip,layer);x,y=q[:2];z=q[2] if volume else 0
                require(0<=z<a.shape[0] and 0<=y<a.shape[1] and 0<=x<a.shape[2],'out of bounds image load')
                v=a[z,y,x];used.add((t.base_mip,layer,x,y,z))
            else:
                q=[floatword(x) for x in words];require(all(math.isfinite(x) for x in q),'nonfinite sample coordinate')
                if array:layer+=min(t.layer_count-1,max(0,math.floor(q[2]+.5)))
                if r.op=='image_sample_l':lod=q[nd+(1 if array else 0)]
                elif r.op=='image_sample_lz' or s.mip=='none' or t.base_mip==t.last_mip:lod=0.
                else:
                    require(s.implicit and t.extent,'implicit LOD lacks calibrated complete quad')
                    # Whole quad derivatives of original guest coordinates. Do not
                    # substitute derivatives from a different native fragment.
                    a0=lane&~3;need=tuple(range(a0,a0+4));require(all(k in coords for k in need),'incomplete implicit LOD quad')
                    c=[np.array([floatword(v) for v in coords[k][:nd]],np.float64) for k in need]
                    ext=np.array([max(1,n>>t.base_mip) for n in t.extent[:nd]],np.float64)
                    dx=(c[(lane&2)+1]-c[lane&2])*ext;dy=(c[2+(lane&1)]-c[lane&1])*ext
                    rho=max(float(np.linalg.norm(dx)),float(np.linalg.norm(dy)));lod=math.log2(rho) if rho else -100.
                if r.op=='image_sample':lod+=s.bias
                lod=min(s.max_lod,max(s.min_lod,lod));filt=s.mag if lod<=0 else s.min
                lod=max(0.,min(t.last_mip-t.base_mip,lod)) if s.mip!='none' else 0.
                if s.mip=='linear':m0=math.floor(lod);m1=min(m0+1,t.last_mip-t.base_mip);weight=np.float32(lod-m0)
                else:m0=m1=math.floor(lod+.5);weight=np.float32(0)
                def level(relative):
                    m=t.base_mip+relative;a=t.get(m,layer);D,H,W,_=a.shape;ns=(W,H,D)
                    pos=[np.float32(q[j])*np.float32(ns[j]) if s.normalized else np.float32(q[j]) for j in range(nd)]
                    def tex(x,y,z=0):
                        idx=[self._index(k,n,mode) for k,n,mode in zip((x,y,z),ns,s.address)]
                        if None in idx:return np.asarray(s.border,np.float32)
                        x,y,z=idx;used.add((m,layer,x,y,z));return a[z,y,x]
                    if filt=='nearest':return tex(*[math.floor(k) for k in pos])
                    pos=[np.float32(k-.5) for k in pos];ii=[math.floor(k) for k in pos];ff=[np.float32(k-i) for k,i in zip(pos,ii)]
                    x,y=ii[:2];fx,fy=ff[:2]
                    def plane(z):
                        a0,b0,c0,d0=tex(x,y,z),tex(x+1,y,z),tex(x,y+1,z),tex(x+1,y+1,z)
                        return (a0+(b0-a0)*fx)*(np.float32(1)-fy)+(c0+(d0-c0)*fx)*fy
                    if not volume:return plane(0)
                    z=ii[2];fz=ff[2];v0=plane(z);v1=plane(z+1);return v0+(v1-v0)*fz
                v=level(m0)
                if m1!=m0 and weight!=0:v=v+(level(m1)-v)*weight
            channels=[0.,1.,None,None,*v];values=[channels[k] for k in t.select]
            require(all(math.isfinite(x) for x in values),'nonfinite shader sample')
            out[lane]=tuple(floatbits(values[j]) for j in range(4) if r.dmask>>j&1);support[lane]=sorted(used)
        self.reads.append({'pc':r.pc,'descriptor':desc,'subresources_by_lane':support})
        return out

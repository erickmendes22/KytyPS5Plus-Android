#!/usr/bin/env python3
import unittest
from types import SimpleNamespace
import numpy as np
from p8_scene_textures import *
from p8_image_trap import floatword

class TextureTests(unittest.TestCase):
    def sample(self,t,s,op,xy):
        d=(1,2,3,4,5,6,7,8);p=(1,2,3,4);m=VersionedTextures({d:t},{p:s})
        r=SimpleNamespace(op=op,dim=t.dim,dmask=15,pc=0)
        return tuple(floatword(x) for x in m(r,d,p,{0:tuple(floatbits(x) for x in xy)})[0]),m.reads
    def test_decode(self):
        self.assertTrue(np.allclose(decode_pixels(bytes([255,128]),16,1,1)[0,0,0],[1,128/255,0,1]))
        self.assertTrue(np.array_equal(decode_pixels(struct.pack('<I',(15<<6)|((15<<6)<<11)|((15<<5)<<22)),122,1,1)[0,0,0],[1,1,1,1]))
        self.assertEqual(decode_pixels(bytes(16),137,4,4).shape,(1,4,4,4))
        self.assertEqual(decode_pixels(bytes(16),145,4,4).shape,(1,4,4,4))
        for f in (16,122,97,137):
            with self.assertRaises(ValueError):decode_pixels(b'',f,1,1)
    def test_array_and_lod(self):
        t=Texture({(0,3):np.ones((1,2,2,4),np.float32),(1,3):np.zeros((1,1,1,4),np.float32)},0,1,2,2,'SQ_RSRC_IMG_2D_ARRAY',extent=(2,2,1))
        s=Sampler('linear','linear','linear',('clamp',)*3,max_lod=1)
        val,trace=self.sample(t,s,'image_sample_l',(.5,.5,1,.5));self.assertEqual(val,(.5,)*4)
        self.assertEqual({tuple(x[:2]) for x in trace[0]['subresources_by_lane'][0]},{(0,3),(1,3)})
        with self.assertRaises(ValueError):self.sample(t,s,'image_sample_lz',(.5,.5,0))
    def test_volume(self):
        a=np.array([0,1],np.float32).reshape(2,1,1,1)*np.ones((2,1,1,4),np.float32)
        t=Texture({(0,0):a},dim='SQ_RSRC_IMG_3D',extent=(1,1,2))
        s=Sampler('linear','linear','none',('clamp',)*3)
        self.assertEqual(self.sample(t,s,'image_sample_lz',(.5,.5,.5))[0],(.5,)*4)
    def test_implicit_requires_quad(self):
        t=Texture({(0,0):np.ones((1,2,2,4),np.float32)},last_mip=1,extent=(2,2,1))
        s=Sampler('linear','linear','linear',('clamp',)*3,max_lod=1,implicit=True)
        with self.assertRaises(ValueError):self.sample(t,s,'image_sample',(.5,.5))
    def test_swizzle_border(self):
        t=Texture({(0,0):np.array([[[[.25,.5,.75,1]]]],np.float32)},select=(6,0,1,4))
        s=Sampler('nearest','nearest','none',('border',)*3,border=(.125,.25,.5,1))
        self.assertEqual(self.sample(t,s,'image_sample_lz',(.5,.5))[0],(.75,0,1,.25))
        self.assertEqual(self.sample(t,s,'image_sample_lz',(-1,.5))[0],(.5,0,1,.125))
if __name__=='__main__':unittest.main()

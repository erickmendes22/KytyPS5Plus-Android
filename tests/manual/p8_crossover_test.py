#!/usr/bin/env python3
"""Exercise the causal outcome logic and reject missing controls."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"docs/android/tools"))
from p8_crossover import assess,write_plan,cut_and_final,command_rows
from p8_patch_gfxr import patch

class Crossover(unittest.TestCase):
    def test_controls(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            def data(name,a,b):
                x=p/(name+"-cut");y=p/(name+"-final")
                x.write_bytes(a);y.write_bytes(b)
                return {"cut":x,"final":y}
            a=data("a",b"a",b"d")
            r=data("r",b"b",b"l")
            crossed=data("crossed",b"b",b"l")
            def run(ref,sham,cross):
                return assess({},["cut"],"final",a,ref,a,sham,cross)
            self.assertEqual(run(r,a,crossed)["native_causal_result"],"NATIVE_PRE_BOUNDARY_SUFFICIENT")
            r2=data("r2",b"a",b"l")
            self.assertEqual(run(r2,a,a)["native_causal_result"],"NATIVE_POST_BOUNDARY_DIFFERENTIAL")
            self.assertEqual(run(r,a,data("partial",b"b",b"p"))["native_causal_result"],"NATIVE_PRE_BOUNDARY_CONTRIBUTION")
            self.assertEqual(run(r,a,data("no-effect",b"b",b"d"))["native_causal_result"],"NATIVE_CAUSAL_UNRESOLVED")
            for sham,cross in [(data("bad-sham",b"a",b"x"),crossed),(a,a)]:
                with self.assertRaises(ValueError):run(r,sham,cross)
            self.assertEqual(run(r,a,crossed)["p8"],"BLOCKED_GUEST_PREFIX")
    def test_missing_coverage(self):
        with self.assertRaises(ValueError):cut_and_final([],{})
    def test_patch_anchor(self):
        with self.assertRaises(ValueError):patch("wrong")
    def test_plan_identity(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d); f=p/"bytes";f.write_bytes(b"1234")
            snap={"format":98,"aspect":1,"mip":0,"layer":0,"extent":[1,1],"expected_bytes":4}
            m={"m":{"command":11,"image":20,"snapshot":snap}}
            result=write_plan(p/"plan",m,{"m":f})
            self.assertEqual(result,{(11,20,4)})
            f.write_bytes(b"12")
            with self.assertRaises(ValueError):write_plan(p/"bad",m,{"m":f})

if __name__=="__main__":unittest.main()

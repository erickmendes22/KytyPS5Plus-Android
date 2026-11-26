#!/usr/bin/env python3
"""Exercise the causal outcome logic and reject missing controls."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"docs/android/tools"))
from p8_crossover import assess,write_plan,cut_and_final,command_rows,reference_name,verify_use_markers
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
    def test_distinct_consumers_shared_payload(self):
        base=dict(owner=1,object=2,native=3,version=4,write_order=5,format=98,width=1,height=1,
                  extent=[1,1],mip=0,layer=0,aspect=1,frame=400,role="gbuffer",file="P8_FULL_1.bin")
        a=dict(base,marker="P8_FULL_1",submit=10)
        b=dict(base,marker="P8_FULL_2",submit=11)
        final=dict(base,marker="P8_FULL_3",role="compositor",owner=8,native=9)
        mapping={a["marker"]:{"snapshot":a,"command":2,"submit_index":3},
                 b["marker"]:{"snapshot":b,"command":5,"submit_index":6},
                 final["marker"]:{"snapshot":final,"command":8,"submit_index":9}}
        def use(s,marker):
            return {"kind":"cut_use","marker":marker,"marker_recorded":True,"submit":s["submit"],
                    "stages":[{"valid":True,"images":[{"cut_candidate":True,"access_known":True,
                                "read":True,"snapshots":[s]}]}]}
        events=[use(a,"P8_USE_1"),use(b,"P8_USE_2"),{"kind":"flip","frame":401,"image":final}]
        self.assertEqual(cut_and_final(events,mapping),(["P8_FULL_1","P8_FULL_2"],"P8_FULL_3"))
        self.assertNotEqual(reference_name(a["marker"]),reference_name(b["marker"]))
        def label(i,name):
            return {"index":i,"function":{"name":"vkCmdInsertDebugUtilsLabelEXT",
                      "args":{"pLabelInfo":{"pLabelName":name}}}}
        verify_use_markers(events,[label(4,"P8_USE_1"),label(7,"P8_USE_2")],mapping)
        with self.assertRaises(ValueError):
            verify_use_markers(events,[label(4,"P8_USE_1"),label(5,"P8_USE_2")],mapping)
        with self.assertRaises(ValueError):
            verify_use_markers(events,[label(4,"P8_USE_1")],mapping)
        with self.assertRaises(ValueError):
            cut_and_final([events[0],use(a,"P8_USE_2"),events[2]],mapping)
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

#!/usr/bin/env python3
"""Install the offline causal intervention in the pinned GFXReconstruct replay.
The Android capture layer and original game are never changed by this script.
"""
import argparse
import subprocess
from pathlib import Path
import shutil
PIN = "4401228c791b11fb7ea15b93795cbd1e938307c0"

def replace_once(s,old,new):
    if s.count(old)!=1:
        raise ValueError("upstream patch anchor missing/ambiguous: "+old[:140])
    return s.replace(old,new,1)

def patch(s):
    s=replace_once(s,'#include "util/defines.h"','#include "util/defines.h"\n#include "kyty_p8_splice_vk.h"')
    anchor='    GetDeviceTable(in_commandBuffer)->CmdCopyImageToBuffer(in_commandBuffer, in_srcImage, args.srcImageLayout, in_dstBuffer, args.regionCount, in_pRegions);'
    injection='''    // P8: offline-only, command-indexed counterfactual before the original readback.
    try {
        auto& experiment=kyty_p8_splice::replay();
        if(auto* row=experiment.find(call_info.index)) {
            const auto* image=GetObjectInfoTable().GetVkImageInfo(args.srcImage);
            const auto* command=GetObjectInfoTable().GetVkCommandBufferInfo(args.commandBuffer);
            kyty_p8_splice::check(image && command && !command->in_rendering_scope
                                  && image->parent_id==command->parent_id,"bad replay object identity");
            const auto* device=GetObjectInfoTable().GetVkDeviceInfo(image->parent_id);
            kyty_p8_splice::check(device && device->parent,"missing replay device");
            VkPhysicalDeviceMemoryProperties props{};
            GetInstanceTable(device->parent)->GetPhysicalDeviceMemoryProperties(device->parent,&props);
            kyty_p8_splice::apply(*GetDeviceTable(in_commandBuffer),device->handle,props,
                in_commandBuffer,in_srcImage,args.srcImageLayout,image->usage,image->format,
                image->extent,image->sample_count,image->level_count,image->layer_count,
                args.srcImage,args.regionCount,in_pRegions,*row,experiment.read(*row),
                experiment.allocations);
            experiment.recorded(*row);
        }
    } catch(const std::exception& e) { kyty_p8_splice::fail(e); }

'''
    s=replace_once(s,anchor,injection+anchor)
    anchor='    OverrideDestroyDevice(GetDeviceTable(in_device->handle)->DestroyDevice, in_device, &args.pAllocator);'
    cleanup='''    try {
        kyty_p8_splice::release(*GetDeviceTable(in_device->handle),in_device->handle,
                               kyty_p8_splice::replay().allocations);
    } catch(const std::exception& e) { kyty_p8_splice::fail(e); }
'''
    return replace_once(s,anchor,cleanup+anchor)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("gfxr",type=Path)
    a=p.parse_args()
    actual=subprocess.check_output(["git","-C",str(a.gfxr),"rev-parse","HEAD"],text=True).strip()
    if actual!=PIN: raise SystemExit("wrong GFXReconstruct revision: "+actual)
    source=a.gfxr/"framework/generated/generated_vulkan_replay_consumer.cpp"
    result=patch(source.read_text())
    shutil.copyfile(Path(__file__).with_name("p8_splice_vk.h"),source.with_name("kyty_p8_splice_vk.h"))
    source.write_text(result)
    print("Installed command-indexed causal replay in pinned GFXReconstruct.")

if __name__=="__main__":
    main()

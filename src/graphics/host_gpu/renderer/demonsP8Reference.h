#pragma once
#include "graphics/host_gpu/renderer/demonsP8Capture.h"
#include "graphics/host_gpu/renderer/demonsPrelightTrace.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include <span>
#include <string>

namespace Libs::Graphics::DemonsP8Reference {
inline std::mutex raw_mutex;
inline uint64_t serial=0,total_bytes=0;
inline constexpr uint64_t RawBudget=256ull*1024ull*1024ull;
inline uint32_t TexelBytes(vk::Format format,vk::ImageAspectFlagBits aspect) {
    if(aspect==vk::ImageAspectFlagBits::eStencil)
        return format==vk::Format::eD32SfloatS8Uint?1:0;
    if(aspect==vk::ImageAspectFlagBits::eDepth)
        return format==vk::Format::eD32Sfloat||format==vk::Format::eD32SfloatS8Uint?4:0;
    switch(format){
        case vk::Format::eR32Uint:
        case vk::Format::eA2B10G10R10UnormPack32:
        case vk::Format::eA2R10G10B10UnormPack32:
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb:
        case vk::Format::eB8G8R8A8Srgb:return 4;
        case vk::Format::eR16G16B16A16Sfloat:return 8;
        default:return 0;
    }
}
inline void WriteEvent(const nlohmann::json& event){
    std::scoped_lock lock(raw_mutex);
    std::ofstream file(DemonsP8Capture::Folder()/"observations.jsonl",std::ios::app);
    if(file){file<<event.dump()<<'\n';file.flush();}
}
inline nlohmann::json Snapshot(RenderContext& context,Image& image,uint32_t mip,uint32_t layer,
                                vk::ImageAspectFlagBits aspect,int32_t frame,uint64_t submit,
                                const std::string& role,uint32_t queue=UINT32_MAX){
    auto event=DemonsPrelightTrace::Identity(image);
    event["kind"]="snapshot";event["frame"]=frame;event["submit"]=submit;
    event["role"]=role;event["queue"]=queue;event["layer"]=layer;event["mip"]=mip;
    event["aspect"]=static_cast<uint32_t>(aspect);event["valid"]=false;
    auto fail=[&](const char* why){event["reason"]=why;WriteEvent(event);return event;};
    if(!DemonsP8Capture::FullFrame(frame))return fail("outside_target_frame");
    const auto& b=image.backing;
    const uint32_t size=TexelBytes(b.format,aspect);
    if(!size||!b.image||b.samples!=1||b.image_type!=vk::ImageType::e2D||
       !(b.usage&vk::ImageUsageFlagBits::eTransferSrc)||mip>=b.mip_levels||layer>=b.layers)
        return fail("unsupported_backing");
    const auto w=std::max(b.extent.width>>mip,1u),h=std::max(b.extent.height>>mip,1u);
    if(w>16384||h>16384)return fail("oversize_extent");
    const auto bytes=uint64_t(w)*h*size;
    if(!bytes||bytes>48ull*1024*1024)return fail("oversize_readback");
    if(!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdInsertDebugUtilsLabelEXT)return fail("marker_unavailable");
    uint64_t id=0;
    {
        std::scoped_lock lock(raw_mutex);
        if(total_bytes<=RawBudget&&bytes<=RawBudget-total_bytes){total_bytes+=bytes;id=++serial;}
    }
    if(!id)return fail("raw_budget_exceeded");
    const auto marker="P8_FULL_"+std::to_string(id);
    const auto filename=marker+".bin";
    event["marker"]=marker;event["file"]=filename;event["extent"]={w,h};
    event["texel_bytes"]=size;event["expected_bytes"]=bytes;
    auto& download=context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
    auto [mapped,offset]=download.Map(bytes,16u,true);
    if(!mapped)return fail("map_failed");
    download.Commit();
    vk::BufferImageCopy copy{};
    copy.bufferOffset=offset;
    copy.imageSubresource={aspect,mip,layer,1};
    copy.imageExtent={w,h,1};
    auto& scheduler=context.GetCommandScheduler();
    scheduler.EndRendering(PassEnd::ImageCopy);
    vk::DebugUtilsLabelEXT label{};
    label.pLabelName=marker.c_str();
    scheduler.Current().Handle().insertDebugUtilsLabelEXT(label);
    event["tick"]=scheduler.CurrentTick();
    image.Download(std::span{&copy,1u},download.Handle(),offset,bytes);
    scheduler.Finish();
    download.Invalidate(offset,bytes);
    event["version_after"]=image.ContentVersion();
    if(event["version"]!=event["version_after"])return fail("version_changed");
    std::ofstream file(DemonsP8Capture::Folder()/filename,std::ios::binary|std::ios::trunc);
    if(!file)return fail("file_open_failed");
    file.write(reinterpret_cast<const char*>(mapped),static_cast<std::streamsize>(bytes));
    file.close();
    if(!file.good())return fail("file_write_failed");
    event["captured_bytes"]=bytes;event["valid"]=true;
    WriteEvent(event);return event;
}
}

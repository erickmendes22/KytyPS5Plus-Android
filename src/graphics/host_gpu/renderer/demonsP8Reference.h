#pragma once
#include "graphics/host_gpu/renderer/demonsP8Capture.h"
#include "graphics/host_gpu/renderer/demonsP8RawStore.h"
#include "graphics/host_gpu/renderer/demonsPrelightTrace.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shader.h"
#include <fmt/format.h>
#include <span>
#include <string>
#include <map>
#include <tuple>

namespace Libs::Graphics::DemonsP8Reference {
inline std::mutex raw_mutex;
// Bounded raw-only capture. Reusing an observation of a clean, unchanged
// version is not a new physical transfer for native replay.
inline RawStore raw_store{DemonsP8CapturePolicy::RawStorageBudget,
    DemonsP8CapturePolicy::RawCopyBudget,DemonsP8CapturePolicy::CriticalStorageBudget,
    DemonsP8CapturePolicy::CriticalCopyBudget};
using ObservationKey=std::tuple<uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>;
inline std::map<ObservationKey,nlohmann::json> clean_observations;
inline uint32_t TexelBytes(vk::Format format,vk::ImageAspectFlagBits aspect) {
    if(aspect==vk::ImageAspectFlagBits::eStencil)
        return format==vk::Format::eD32SfloatS8Uint?1:0;
    if(aspect==vk::ImageAspectFlagBits::eDepth)
        return format==vk::Format::eD32Sfloat||format==vk::Format::eD32SfloatS8Uint?4:0;
    switch(format){
        case vk::Format::eR16Sfloat:return 2;
        case vk::Format::eR32G32Sfloat:return 8;
        case vk::Format::eR16G16Sfloat:
        case vk::Format::eR32Sfloat:
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
    if(!DemonsP8CapturePolicy::CaptureCriticalFrame(DemonsP8Capture::EvidenceFrame(frame),
        DemonsP8Capture::FullFrame(frame),role.c_str()))return fail("outside_target_frame");
    const bool critical=DemonsP8CapturePolicy::CriticalRole(role.c_str());
    event["critical"]=critical;
    const auto& b=image.backing;
    const uint32_t size=TexelBytes(b.format,aspect);
    if(!size||!b.image||b.samples!=1||b.image_type!=vk::ImageType::e2D||
       !(b.usage&vk::ImageUsageFlagBits::eTransferSrc)||mip>=b.mip_levels||layer>=b.layers)
        return fail("unsupported_backing");
    const auto w=std::max(b.extent.width>>mip,1u),h=std::max(b.extent.height>>mip,1u);
    if(w>16384||h>16384)return fail("oversize_extent");
    const auto bytes=uint64_t(w)*h*size;
    if(!bytes||bytes>48ull*1024*1024)return fail("oversize_readback");
    event["native_replay"]=false;
    const ObservationKey key={image.info.data.address,
        event["object"].get<uint64_t>(),event["native"].get<uint64_t>(),
        image.ContentVersion(),image.gpu_write_order,mip,layer,static_cast<uint32_t>(aspect),
        static_cast<uint32_t>(b.format),w,h};
    const bool clean=role!="q8_hdr_before"&&role!="q8_hdr_after"&&DemonsP8CapturePolicy::Reusable(image.IsCpuDirty(),image.IsBufferModified(),true,true,true);
    if(clean){
        nlohmann::json prior;
        {
            std::scoped_lock lock(raw_mutex);
            const auto it=clean_observations.find(key);
            if(it!=clean_observations.end())prior=it->second;
        }
        if(!prior.is_null()){
            event["extent"]={w,h};event["texel_bytes"]=size;event["expected_bytes"]=bytes;
            event["file"]=prior["file"];event["captured_bytes"]=bytes;
            event["source_observation"]=prior["marker"];
            event["source_submit"]=prior["submit"];
            event["version_after"]=image.ContentVersion();
            event["observation_reused"]=true;event["valid"]=true;
            event["marker_recorded"]=false;
            WriteEvent(event);return event;
        }
    }
    const auto id=raw_store.ReserveCopy(bytes,critical);
    if(!id)return fail("raw_copy_budget_exceeded");
    const auto marker="P8_FULL_"+std::to_string(id);
    event["marker"]=marker;event["extent"]={w,h};
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
    const bool markers=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdInsertDebugUtilsLabelEXT!=nullptr;
    if(markers)scheduler.Current().Handle().insertDebugUtilsLabelEXT(label);
    event["marker_recorded"]=markers;
    event["tick"]=scheduler.CurrentTick();
    image.Download(std::span{&copy,1u},download.Handle(),offset,bytes);
    if(role=="q8_hdr_before"||role=="q8_hdr_after")scheduler.Wait(scheduler.CurrentTick());
    else scheduler.Finish();
    download.Invalidate(offset,bytes);
    event["version_after"]=image.ContentVersion();
    if(event["version"]!=event["version_after"])return fail("version_changed");
    const auto payload=raw_store.Save(DemonsP8Capture::Folder(),id,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(mapped),static_cast<size_t>(bytes)},critical);
    if(payload.reason)return fail(payload.reason);
    event["file"]=payload.file;event["payload_deduplicated"]=payload.reused;
    event["captured_bytes"]=bytes;event["valid"]=true;event["observation_reused"]=false;
    if(clean&&!image.IsCpuDirty()&&!image.IsBufferModified()&&event["write_order"]==image.gpu_write_order){
        std::scoped_lock lock(raw_mutex);
        clean_observations.emplace(key,event);
    }
    WriteEvent(event);return event;
}

// Raw-only observations distinguish same-version reuse from a physical transfer.
inline std::atomic<uint64_t> use_serial{0};
inline std::atomic<uint64_t> radiance_serial{0};
inline nlohmann::json SnapshotBoundary(RenderContext& context,Image& image,
    uint32_t mip,uint32_t layer,vk::ImageAspectFlagBits aspect,int32_t frame,
    uint64_t submit,const std::string& role,uint32_t queue=UINT32_MAX) {
    return Snapshot(context,image,mip,layer,aspect,frame,submit,role,queue);
}

// Use the same stage labels as PipelineCache::StageLabels and the exact
// bound program hash. An unknown or synthesized program is not evidence
// of guest code coverage.
inline const char* GuestStageName(ShaderType stage) {
    switch(stage) {
        case ShaderType::Vertex: return "vs";
        case ShaderType::Mesh: return "ms";
        case ShaderType::Local: return "ls";
        case ShaderType::TessellationControl: return "hs";
        case ShaderType::TessellationEvaluation: return "ds";
        case ShaderType::Pixel: return "ps";
        case ShaderType::Compute: return "cs";
        default: return nullptr;
    }
}
template<typename Bound>
inline nlohmann::json InspectBindings(RenderContext& context,const Bound& bound,
    int32_t frame,uint64_t submit) {
    nlohmann::json stage={{"images",nlohmann::json::array()},{"buffers",nlohmann::json::array()}};
    if(!bound.runtime||!bound.runtime->program){
        stage["valid"]=false;stage["reason"]="missing_runtime";return stage;
    }
    const auto& program=*bound.runtime->program;
    stage["shader"]=program.shader_hash;
    if(const auto* name=GuestStageName(program.stage); name && program.shader_hash) {
        stage["guest_file"]=fmt::format("guest-shaders/{}_{:016x}_probe.bin",name,program.shader_hash);
    } else {
        stage["guest_file_reason"]="unsupported_guest_stage_or_hash";
    }
    stage["valid"]=true;
    stage["bda_writes"]=program.info.bda_writes;
    stage["uses_dma"]=program.info.uses_dma;
    stage["uses_gds"]=program.info.uses_gds;
    // Preserve raw scalar reads and descriptor words for the replay targets.
    const bool replay=program.shader_hash==0xe3daeb1e7426761bull||program.shader_hash==0x3dfe402653925fbfull||DemonsP8CapturePolicy::HzbShader(program.shader_hash)||
        DemonsP8CapturePolicy::ClusterShader(program.shader_hash)||
        program.shader_hash==DemonsP8CapturePolicy::CompositePS||
        program.shader_hash==DemonsP8CapturePolicy::DirectScenePS||
        program.shader_hash==0xe86bfee195efa5a4ull||program.shader_hash==0x8f52cca9ea3ba4fcull;
    if(replay&&bound.runtime->resources){
        const auto& snap=*bound.runtime->resources;
        auto& raw=stage["guest_operands"];
        raw={{"user_data",snap.user_data},{"user_data_base",snap.diagnostic_user_data_base},
            {"scalar_complete",snap.diagnostic_scalar_complete},{"srt",snap.flattened_srt},
            {"scalar_regions",nlohmann::json::array()},{"image_descriptors",nlohmann::json::array()},
            {"sampler_descriptors",nlohmann::json::array()},{"buffer_descriptors",nlohmann::json::array()}};
        for(const auto& r:snap.diagnostic_scalar_reads)
            raw["scalar_regions"].push_back({{"address",r.address},{"words",r.words},{"kind",r.kind}});
        for(const auto& r:snap.images)raw["image_descriptors"].push_back(r.dwords);
        for(const auto& r:snap.samplers)raw["sampler_descriptors"].push_back(r.dwords);
        for(const auto& r:snap.buffers)raw["buffer_descriptors"].push_back(r.dwords);
    }
    for(size_t i=0;i<bound.buffer_sources.size();++i){
        const auto& b=bound.buffer_sources[i];
        bool known=false,written=false;
        for(const auto& resource:program.info.buffers)if(resource.descriptor_index==i){
            known=true;written|=resource.written;
        }
        nlohmann::json row={{"slot",i},{"guest",b.address},{"bytes",b.size},
            {"access_known",known},{"read_conservative",true},{"written",written}};
        if(i<bound.buffers.size()){
            const auto& native=bound.buffers[i];
            row["native"]=std::hash<VkBuffer>{}(static_cast<VkBuffer>(native.buffer));
            row["native_offset"]=native.offset;row["native_range"]=native.range;
        }
        stage["buffers"].push_back(std::move(row));
    }
    for(size_t i=0;i<bound.images.size();++i){
        const auto& b=bound.images[i];
        const bool known=i<program.info.images.size();
        const bool read=known&&program.info.images[i].read;
        const bool written=known&&program.info.images[i].written;
        nlohmann::json entry={{"slot",i},{"guest",b.desc.info.data.address},
            {"bytes",b.desc.info.data.size},{"placeholder",b.placeholder},
            {"access_known",known},{"read",read},{"written",written},
            {"snapshots",nlohmann::json::array()}};
        const bool guest_material=DemonsPrelightTrace::Matches(b.desc.info.data.address,b.desc.info.data.size);
        const bool guest_depth=b.desc.info.data.address==0x282b60000ull;
        entry["cut_candidate"]=guest_material||guest_depth;
        if(!b.image_id||b.placeholder){
            if(entry["cut_candidate"].get<bool>()&&(read||!known))entry["reason"]="missing_binding";
            stage["images"].push_back(std::move(entry));continue;
        }
        auto& image=context.GetTextureCache().GetImage(b.image_id);
        entry["image"]=DemonsPrelightTrace::Identity(image);
        const bool material=guest_material||DemonsPrelightTrace::Matches(image.info.data.address,image.info.data.size);
        const bool depth=guest_depth||image.info.data.address==0x282b60000ull;
        entry["cut_candidate"]=material||depth;
        const auto view=std::ranges::find(image.views,b.image_view,&CachedImageView::view);
        if(view==image.views.end()){
            entry["reason"]="missing_view";stage["images"].push_back(std::move(entry));continue;
        }
        const auto& v=view->info;
        entry["view"]={{"mip",v.base_level},{"levels",v.level_count},{"layer",v.base_layer},
            {"layers",v.layer_count},{"aspect",static_cast<uint32_t>(v.aspect)},
            {"format",static_cast<uint32_t>(v.format)}};
        // Preserve every actual potentially read material/depth subresource.
        // Unknown access classification is retained as incomplete, not ignored.
        if((material||depth)&&(read||!known)){
            const auto& backing=image.backing;
            if(!v.level_count||!v.layer_count||v.base_level>=backing.mip_levels||
               v.base_layer>=backing.layers||v.level_count>backing.mip_levels-v.base_level||
               v.layer_count>backing.layers-v.base_layer){
                entry["reason"]="unsupported_view_range";
            }else{
                for(uint32_t m=0;m<v.level_count;++m)for(uint32_t l=0;l<v.layer_count;++l){
                    const auto capture=[&](vk::ImageAspectFlagBits aspect,const char* role){
                        entry["snapshots"].push_back(SnapshotBoundary(context,image,v.base_level+m,
                            v.base_layer+l,aspect,frame,submit,role));
                    };
                    if(v.aspect&vk::ImageAspectFlagBits::eColor)capture(vk::ImageAspectFlagBits::eColor,material?"gbuffer":"depth");
                    if(v.aspect&vk::ImageAspectFlagBits::eDepth)capture(vk::ImageAspectFlagBits::eDepth,"depth");
                    if(v.aspect&vk::ImageAspectFlagBits::eStencil)capture(vk::ImageAspectFlagBits::eStencil,"stencil");
                }
            }
        }
        if(DemonsP8CapturePolicy::HzbShader(program.shader_hash)&&read&&!written){
            const bool one=v.level_count==1&&v.layer_count==1;
            if(one&&(v.aspect==vk::ImageAspectFlagBits::eColor||v.aspect==vk::ImageAspectFlagBits::eDepth)){
                entry["snapshots"].push_back(Snapshot(context,image,v.base_level,v.base_layer,
                    v.aspect==vk::ImageAspectFlagBits::eDepth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,
                    frame,submit,"hzb_input"));
            }else entry["reason"]="unsupported_hzb_view";
        }
        const auto* role=DemonsP8CapturePolicy::RadianceInputRole(program.shader_hash,
            image.info.data.address,image.IsGpuModified(),read,written,v.level_count,
            v.layer_count,image.info.data.size);
        if(role){
            // Full actual view for supported, bounded 2D radiance operands.
            // Snapshot validates backing and global raw transfer/storage caps.
            entry["radiance_role"]=role;
            if(v.aspect==vk::ImageAspectFlagBits::eColor&&v.base_level<image.backing.mip_levels&&
               v.level_count<=image.backing.mip_levels-v.base_level&&v.base_layer<image.backing.layers){
                for(uint32_t m=0;m<v.level_count;++m)
                    entry["snapshots"].push_back(SnapshotBoundary(context,image,v.base_level+m,
                        v.base_layer,vk::ImageAspectFlagBits::eColor,frame,submit,role));
            }else entry["radiance_reason"]="unsupported_view_range";
        }
        stage["images"].push_back(std::move(entry));
    }
    return stage;
}
inline std::string MarkUse(vk::CommandBuffer command,nlohmann::json event) {
    if(event.is_null())return {};
    const auto marker="P8_USE_"+std::to_string(++use_serial);
    event["kind"]="cut_use";event["marker"]=marker;event["native_replay"]=false;
    event["guest_packet"]=DemonsP8Capture::current_packet;
    if(command&&VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdInsertDebugUtilsLabelEXT){
        vk::DebugUtilsLabelEXT label{};label.pLabelName=marker.c_str();
        command.insertDebugUtilsLabelEXT(label);
        event["marker_recorded"]=true;
    }else event["marker_recorded"]=false;
    WriteEvent(event);
    return marker;
}
// Call after all commands using the previous VkCommandBuffer and its barrier.
// A separate event ties the actual produced subresource to the exact use.
template<typename Bound>
inline void CaptureHzbOutput(RenderContext& context,const Bound& bound,int32_t frame,
    uint64_t submit,const std::string& marker){
    if(marker.empty()||!bound.runtime||!bound.runtime->program||
       !DemonsP8CapturePolicy::HzbShader(bound.runtime->program->shader_hash))return;
    const auto& program=*bound.runtime->program;
    nlohmann::json event={{"kind","hzb_after"},{"frame",frame},{"submit",submit},
        {"use",marker},{"shader",program.shader_hash},{"outputs",nlohmann::json::array()}};
    for(size_t i=0;i<bound.images.size();++i){
        const auto& b=bound.images[i];
        if(i>=program.info.images.size()||!program.info.images[i].written)continue;
        nlohmann::json row={{"slot",i},{"valid",false}};
        if(b.image_id&&!b.placeholder){
            auto& image=context.GetTextureCache().GetImage(b.image_id);
            const auto view=std::ranges::find(image.views,b.image_view,&CachedImageView::view);
            if(view!=image.views.end()&&view->info.level_count==1&&view->info.layer_count==1&&
               view->info.aspect==vk::ImageAspectFlagBits::eColor){
                row["snapshot"]=Snapshot(context,image,view->info.base_level,view->info.base_layer,
                    vk::ImageAspectFlagBits::eColor,frame,submit,"hzb_output");
                row["valid"]=row["snapshot"].value("valid",false);
            }
        }
        event["outputs"].push_back(std::move(row));
    }
    WriteEvent(event);
}
}

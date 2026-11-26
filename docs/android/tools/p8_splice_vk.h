// Offline replay only. No code from this header is linked into the APK.
// A transfer-side intervention: replace exactly one captured image subresource
// immediately before its original diagnostic readback, then restore the layout.
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kyty_p8_splice {
struct Row {
    uint64_t command{}, image{};
    uint32_t format{}, aspect{}, mip{}, layer{}, width{}, height{};
    uint64_t bytes{};
    std::string path;
    bool used=false;
};
inline void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
inline void vkcheck(VkResult result, const char* why) { check(result==VK_SUCCESS,why); }

inline uint32_t pixel_bytes(VkFormat fmt, VkImageAspectFlags a) {
    if (a==VK_IMAGE_ASPECT_DEPTH_BIT)
        return fmt==VK_FORMAT_D32_SFLOAT || fmt==VK_FORMAT_D32_SFLOAT_S8_UINT ? 4 : 0;
    if (a==VK_IMAGE_ASPECT_STENCIL_BIT)
        return fmt==VK_FORMAT_D32_SFLOAT_S8_UINT || fmt==VK_FORMAT_S8_UINT ? 1 : 0;
    if (a!=VK_IMAGE_ASPECT_COLOR_BIT) return 0;
    switch(fmt) {
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        default: return 0;
    }
}

inline std::map<uint64_t,Row> read_plan(std::istream& in) {
    std::map<uint64_t,Row> rows;
    std::string line;
    check(bool(std::getline(in,line)) && line=="KYTY_P8_SPLICE_V1","bad splice header");
    uint64_t total=0;
    while (std::getline(in,line)) {
        if(line.empty()) continue;
        Row r;
        std::istringstream is(line);
        check(bool(is>>r.command>>r.image>>r.format>>r.aspect>>r.mip>>r.layer
                 >>r.width>>r.height>>r.bytes>>r.path),"bad splice row");
        std::string tail;
        check(!(is>>tail),"trailing splice row fields");
        check(r.command && r.image && r.width && r.height && r.width<=16384 && r.height<=16384
              && r.mip<32 && r.layer<2048,"bad splice identity");
        const auto bpp=pixel_bytes(static_cast<VkFormat>(r.format),r.aspect);
        check(bpp && r.bytes==uint64_t(r.width)*r.height*bpp,"bad splice size");
        check(r.bytes<=64ull*1024*1024 && total+r.bytes<=1024ull*1024*1024,"splice budget");
        total+=r.bytes;
        check(rows.emplace(r.command,std::move(r)).second,"duplicate splice command");
    }
    return rows;
}

struct Allocation { VkDevice device{}; VkBuffer buffer{}; VkDeviceMemory memory{}; };

// All injected buffers remain live until the replay device is idle. The source
// bytes are immutable; a later host write cannot race an earlier submission.
template<class Table>
void release(Table& t, VkDevice device, std::vector<Allocation>& allocations) {
    if(std::none_of(allocations.begin(),allocations.end(),
                    [&](const auto& a){return a.device==device;})) return;
    vkcheck(t.DeviceWaitIdle(device),"splice wait idle");
    for(auto it=allocations.begin();it!=allocations.end();) {
        if(it->device!=device){++it;continue;}
        t.DestroyBuffer(device,it->buffer,nullptr);
        t.FreeMemory(device,it->memory,nullptr);
        it=allocations.erase(it);
    }
}

template<class Table>
void apply(Table& t, VkDevice device, const VkPhysicalDeviceMemoryProperties& mem,
           VkCommandBuffer cb, VkImage image, VkImageLayout layout,
           VkImageUsageFlags usage, VkFormat format, VkExtent3D full_extent,
           VkSampleCountFlagBits samples, uint32_t levels, uint32_t layers,
           uint64_t captured_image, uint32_t count, const VkBufferImageCopy* regions,
           Row& row, const std::vector<uint8_t>& data, std::vector<Allocation>& allocations) {
    check(!row.used && captured_image==row.image && format==static_cast<VkFormat>(row.format),
          "splice image identity changed");
    check(image && cb && device && count==1 && regions && samples==VK_SAMPLE_COUNT_1_BIT
          && row.mip<levels && row.layer<layers && full_extent.depth==1,
          "unsupported splice image");
    check((usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)!=0,"image lacks TRANSFER_DST");
    check(layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || layout==VK_IMAGE_LAYOUT_GENERAL,
          "unsupported copy source layout");
    const auto& c=regions[0];
    check(c.imageSubresource.aspectMask==row.aspect && c.imageSubresource.mipLevel==row.mip
          && c.imageSubresource.baseArrayLayer==row.layer && c.imageSubresource.layerCount==1
          && c.imageOffset.x==0 && c.imageOffset.y==0 && c.imageOffset.z==0
          && c.imageExtent.width==row.width && c.imageExtent.height==row.height && c.imageExtent.depth==1
          && !c.bufferRowLength && !c.bufferImageHeight
          && row.width==std::max(1u,full_extent.width>>row.mip)
          && row.height==std::max(1u,full_extent.height>>row.mip)
          && data.size()==row.bytes,"splice subresource/length mismatch");
    // Combined depth/stencil requires a joint transition unless separate
    // layouts are enabled. GENERAL can be copied without changing either
    // layout; otherwise refuse rather than fabricate the other aspect's state.
    check(format!=VK_FORMAT_D32_SFLOAT_S8_UINT || layout==VK_IMAGE_LAYOUT_GENERAL,
          "combined depth/stencil transfer layout not certified");
    VkBufferCreateInfo bi{};
    bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size=row.bytes;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    Allocation a{device};
    vkcheck(t.CreateBuffer(device,&bi,nullptr,&a.buffer),"splice create staging");
    VkMemoryRequirements req{};
    t.GetBufferMemoryRequirements(device,a.buffer,&req);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<mem.memoryTypeCount;++i)
        if((req.memoryTypeBits&(1u<<i)) &&
           (mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type=i;
            if(mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
        }
    if(type==UINT32_MAX){t.DestroyBuffer(device,a.buffer,nullptr);check(false,"no host visible splice memory");}
    VkMemoryAllocateInfo ai{};
    ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
    if(t.AllocateMemory(device,&ai,nullptr,&a.memory)!=VK_SUCCESS) {
        t.DestroyBuffer(device,a.buffer,nullptr);check(false,"splice allocate");
    }
    // Retain allocation on subsequent failures as well. The process exits
    // without publishing an audit success if an operation fails.
    allocations.push_back(a);
    vkcheck(t.BindBufferMemory(device,a.buffer,a.memory,0),"splice bind");
    void* mapped=nullptr;
    vkcheck(t.MapMemory(device,a.memory,0,VK_WHOLE_SIZE,0,&mapped),"splice map");
    std::memcpy(mapped,data.data(),data.size());
    if(!(mem.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange mr{};
        mr.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; mr.memory=a.memory; mr.size=VK_WHOLE_SIZE;
        vkcheck(t.FlushMappedMemoryRanges(device,1,&mr),"splice flush");
    }
    t.UnmapMemory(device,a.memory);

    const auto dst_layout=layout==VK_IMAGE_LAYOUT_GENERAL?layout:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier b{};
    b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout=layout; b.newLayout=dst_layout;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image;
    b.subresourceRange={row.aspect,row.mip,1,row.layer,1};
    // In GENERAL, combined D/S stays GENERAL; use the whole combined
    // subresource in its barrier to avoid a separate-layout assumption.
    if(format==VK_FORMAT_D32_SFLOAT_S8_UINT)
        b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    VkBufferMemoryBarrier bb{};
    bb.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; bb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    bb.buffer=a.buffer; bb.size=VK_WHOLE_SIZE;
    t.CmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&bb,1,&b);
    auto upload=c; upload.bufferOffset=0;
    t.CmdCopyBufferToImage(cb,a.buffer,image,dst_layout,1,&upload);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout=dst_layout; b.newLayout=layout;
    t.CmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,nullptr,0,nullptr,1,&b);
    row.used=true;
}

struct Replay {
    std::map<uint64_t,Row> rows;
    std::vector<Allocation> allocations;
    std::string audit;
    Replay() {
        const char* path=std::getenv("KYTYP8_SPLICE_PLAN");
        if(!path||!*path)return;
        const char* out=std::getenv("KYTYP8_SPLICE_AUDIT");
        check(out&&*out,"missing splice audit path");
        audit=out;
        std::ofstream(audit,std::ios::trunc);
        std::ifstream input(path);
        check(bool(input),"cannot open splice plan");
        rows=read_plan(input);
        check(!rows.empty(),"empty splice intervention");
    }
    Row* find(uint64_t cmd) {
        auto it=rows.find(cmd);return it==rows.end()?nullptr:&it->second;
    }
    std::vector<uint8_t> read(const Row& r) {
        std::ifstream f(r.path,std::ios::binary|std::ios::ate);
        check(bool(f)&&f.tellg()==static_cast<std::streamoff>(r.bytes),"bad splice payload");
        std::vector<uint8_t> data(static_cast<size_t>(r.bytes));
        f.seekg(0);
        check(bool(f.read(reinterpret_cast<char*>(data.data()),data.size())),"splice read");
        return data;
    }
    void recorded(const Row& r) {
        std::ofstream f(audit,std::ios::app);
        check(bool(f),"splice audit open");
        f<<r.command<<" "<<r.image<<" "<<r.bytes<<"\n";
        check(bool(f.flush()),"splice audit flush");
    }
};
inline Replay& replay() {
    static Replay r;
    return r;
}
[[noreturn]] inline void fail(const std::exception& e) {
    std::fprintf(stderr,"P8 splice: %s\n",e.what());
    std::exit(72);
}
} // namespace kyty_p8_splice

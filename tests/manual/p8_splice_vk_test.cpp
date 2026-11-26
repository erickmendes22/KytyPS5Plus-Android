// Actual Vulkan transfer smoke test for the offline P8 intervention.
#include "p8_splice_vk.h"
#include <array>
#include <iostream>
#include <vector>
#define VK_ENTRY(x) PFN_vk##x x=vk##x
struct Dispatch {
    VK_ENTRY(DeviceWaitIdle);
    VK_ENTRY(DestroyBuffer);
    VK_ENTRY(FreeMemory);
    VK_ENTRY(CreateBuffer);
    VK_ENTRY(GetBufferMemoryRequirements);
    VK_ENTRY(AllocateMemory);
    VK_ENTRY(BindBufferMemory);
    VK_ENTRY(MapMemory);
    VK_ENTRY(FlushMappedMemoryRanges);
    VK_ENTRY(UnmapMemory);
    VK_ENTRY(CmdPipelineBarrier);
    VK_ENTRY(CmdCopyBufferToImage);
};
#undef VK_ENTRY
using namespace kyty_p8_splice;

int main() {
    // Strict parsing is exercised before any GPU work.
    std::istringstream plan("KYTY_P8_SPLICE_V1\n10 20 98 1 0 0 2 2 16 example.bin\n");
    auto rows=read_plan(plan);
    check(rows.size()==1 && rows.at(10).bytes==16,"plan parse");
    for(const auto* bad:{"WRONG\n","KYTY_P8_SPLICE_V1\n10 20 98 1 0 0 2 2 15 f\n",
                          "KYTY_P8_SPLICE_V1\n10 20 98 1 0 0 2 2 16 a\n10 20 98 1 0 0 2 2 16 b\n"}) {
        bool rejected=false;
        try { std::istringstream input(bad); (void)read_plan(input); }
        catch(const std::exception&) { rejected=true; }
        check(rejected,"invalid plan admitted");
    }
    VkApplicationInfo app{};
    app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&app;
    VkInstance instance{};
    vkcheck(vkCreateInstance(&ici,nullptr,&instance),"instance");
    uint32_t n=0;
    vkcheck(vkEnumeratePhysicalDevices(instance,&n,nullptr),"enumerate");
    check(n>0,"no test Vulkan device");
    std::vector<VkPhysicalDevice> physical(n);
    vkcheck(vkEnumeratePhysicalDevices(instance,&n,physical.data()),"physical");
    auto gpu=physical.front();
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count;++i)
        if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    check(family!=UINT32_MAX,"queue family");
    float priority=1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex=family;
    qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{};
    di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkDevice dev{};
    vkcheck(vkCreateDevice(gpu,&di,nullptr,&dev),"device");
    VkQueue queue{};vkGetDeviceQueue(dev,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
    auto memory_type=[&](uint32_t mask,VkMemoryPropertyFlags flags){
        for(uint32_t i=0;i<mp.memoryTypeCount;++i)
            if((mask&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("no compatible smoke memory");
    };
    VkImageCreateInfo ii{};
    ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;
    ii.format=VK_FORMAT_R32_UINT;ii.extent={2,2,1};ii.mipLevels=1;ii.arrayLayers=1;
    ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image{};
    vkcheck(vkCreateImage(dev,&ii,nullptr,&image),"image");
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(dev,image,&req);
    VkMemoryAllocateInfo ai{};
    ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;
    ai.memoryTypeIndex=memory_type(req.memoryTypeBits,0);
    VkDeviceMemory imem{};vkcheck(vkAllocateMemory(dev,&ai,nullptr,&imem),"image memory");
    vkcheck(vkBindImageMemory(dev,image,imem,0),"bind image");
    VkBufferCreateInfo bi{};
    bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=16;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer readback{};vkcheck(vkCreateBuffer(dev,&bi,nullptr,&readback),"readback");
    vkGetBufferMemoryRequirements(dev,readback,&req);
    ai.allocationSize=req.size;
    ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory rmem{};vkcheck(vkAllocateMemory(dev,&ai,nullptr,&rmem),"readback memory");
    vkcheck(vkBindBufferMemory(dev,readback,rmem,0),"bind readback");
    VkCommandPoolCreateInfo pi{};
    pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=family;
    pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};vkcheck(vkCreateCommandPool(dev,&pi,nullptr,&pool),"pool");
    VkCommandBufferAllocateInfo cai{};
    cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cai.commandPool=pool;
    cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    VkCommandBuffer cb{};vkcheck(vkAllocateCommandBuffers(dev,&cai,&cb),"cmd");
    VkCommandBufferBeginInfo cbi{};cbi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    auto begin=[&]{vkcheck(vkResetCommandBuffer(cb,0),"reset");vkcheck(vkBeginCommandBuffer(cb,&cbi),"begin");};
    auto submit=[&]{
        vkcheck(vkEndCommandBuffer(cb),"end");
        VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount=1;si.pCommandBuffers=&cb;
        vkcheck(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"submit");
        vkcheck(vkQueueWaitIdle(queue),"wait");
    };
    VkImageMemoryBarrier barrier{};
    barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    begin();
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    VkClearColorValue clear{};clear.uint32[0]=0x12345678;
    vkCmdClearColorImage(cb,image,barrier.newLayout,&clear,1,&barrier.subresourceRange);
    barrier.oldLayout=barrier.newLayout;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    submit();
    VkBufferImageCopy region{};
    region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={2,2,1};
    Dispatch table;
    std::vector<Allocation> allocations;
    auto trial=[&](const std::vector<uint8_t>* replacement){
        begin();
        if(replacement) {
            auto row=rows.at(10);
            apply(table,dev,mp,cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,ii.usage,ii.format,
                  ii.extent,ii.samples,ii.mipLevels,ii.arrayLayers,20,1,&region,row,*replacement,allocations);
            check(row.used,"intervention not recorded");
        }
        vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&region);
        VkBufferMemoryBarrier b{};
        b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=readback;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,1,&b,0,nullptr);
        submit();
        void* p{};vkcheck(vkMapMemory(dev,rmem,0,16,0,&p),"map");
        std::vector<uint8_t> output(16);std::memcpy(output.data(),p,16);vkUnmapMemory(dev,rmem);
        return output;
    };
    auto observed=trial(nullptr);
    std::array<uint32_t,4> expected{0x12345678,0x12345678,0x12345678,0x12345678};
    check(std::memcmp(observed.data(),expected.data(),16)==0,"baseline pixels");
    check(trial(&observed)==observed,"sham changed data");
    std::vector<uint8_t> replacement{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    check(trial(&replacement)==replacement,"replacement did not reach readback");
    check(trial(nullptr)==replacement,"replacement did not persist to next use");
    bool rejected=false;
    try {
        auto row=rows.at(10);
        apply(table,dev,mp,cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,ii.usage,ii.format,
              ii.extent,ii.samples,1,1,21,1,&region,row,replacement,allocations);
    } catch(const std::exception&) {rejected=true;}
    check(rejected,"wrong native image accepted");
    release(table,dev,allocations);
    check(allocations.empty(),"staging lifetime");
    vkDestroyCommandPool(dev,pool,nullptr);vkDestroyBuffer(dev,readback,nullptr);vkFreeMemory(dev,rmem,nullptr);
    vkDestroyImage(dev,image,nullptr);vkFreeMemory(dev,imem,nullptr);
    vkDestroyDevice(dev,nullptr);vkDestroyInstance(instance,nullptr);
    std::cout<<"P8 Vulkan splice: baseline, sham, replacement, subsequent read, rejected wrong identity PASS\n";
}

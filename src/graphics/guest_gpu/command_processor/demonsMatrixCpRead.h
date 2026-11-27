#include <fmt/format.h>
#pragma once
#include "graphics/host_gpu/renderer/demonsCausalMatrix.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include <cstring>
#include <atomic>
namespace Libs::Graphics::DemonsMatrixCpRead {
// PC ReadGuestForCp-style backing read. Only C1 changes the CPU fallback.
template<class T> T Read(RenderContext& renderer,uint64_t address) {
    T value{};
    if (!GuestGpu::IsGpuThread()) {
        std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T)); return value;
    }
    // C1 explicit coherent read can occur after a prior child queued a
    // record job. The packet-entry drain alone does not cover that child.
    // Reacquire front ownership before touching the renderer cache.
    if(!GpuPipeline::OnBack())GpuPipeline::DrainForFront(GpuPipeline::DrainCause::Other);
    auto& cache=renderer.GetBufferCache();
    cache.StampPendingWrites();
    const bool pending=cache.HasPendingBackingPublication(address,sizeof(T));
    const bool dirty=cache.HasGpuDirtyBytes(address,sizeof(T));
    T before{};
    const bool had=LibKernel::Memory::TryReadBacking(address,&before,sizeof(T));
    if(pending) renderer.GetCommandScheduler().Finish();
    const bool read=LibKernel::Memory::TryReadShaderResourceBacking(address,&value,sizeof(T),true);
    if(!read) std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T));
    const bool changed=had && std::memcmp(&before,&value,sizeof(T))!=0;
    static std::atomic_uint32_t n{0},logs{0};
    const auto i=n.fetch_add(1,std::memory_order_relaxed)+1;
    if((i<=16 || changed || (i & (i-1))==0) && logs.fetch_add(1,std::memory_order_relaxed)<512)
        Log::WriteToConsoleAndLog(fmt::format("[DS_MATRIX_CP] mode=C1 addr=0x{:x} size={} dirty={} pending={} read={} changed={}\n",
            address,sizeof(T),dirty,pending,read,changed));
    return value;
}
}

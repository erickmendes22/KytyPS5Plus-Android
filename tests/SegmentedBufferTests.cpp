#include "common/emulatorConfig.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace Libs::Graphics;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "SegmentedBufferTests: %s\n", message); std::abort(); }
}
void Check(uint32_t bits, bool scalar, bool robust) {
    using namespace ShaderRecompiler::IR;
    Program program;
    program.stage = ShaderType::Compute;
    program.wave_size = 32;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    program.block_storage.push_back(std::make_unique<Block>());
    auto* block = program.block_storage.back().get();
    program.blocks.push_back(block);
    auto& lane = block->AppendNewInst(ValueOpcode::LaneId);
    auto& offset = block->AppendNewInst(ValueOpcode::IMul32, {Value(&lane), Value(16u)});
    auto& handle = block->AppendNewInst(ValueOpcode::GetBufferResource,
        {Value(0u), Value(0u), Value(0u), Value(0u)});
    handle.SetFlags<uint32_t>(0u);
    program.descriptor_sources.resize(1);
    program.descriptor_sources[0].dword_count = 4;
    BufferResource resource;
    resource.source = 0;
    resource.packed_stride = 1;
    resource.chunk_bytes = 128;
    resource.chunk_count = 5; // includes a partial final slice, not just power-of-two lengths.
    resource.read = true;
    resource.written = !scalar;
    resource.scalar = scalar;
    program.info.buffers.push_back(resource);
    program.memory_info.push_back({.kind = scalar ? ResourceKind::ScalarBuffer : ResourceKind::Buffer,
                                   .data_bits = bits, .offen = true});
    if (scalar) {
        auto& load = block->AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&handle), Value(&offset)});
        load.SetFlags(MemoryFlags{0u, 0u});
        block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(&load)});
    } else {
        const auto load_op = bits == 8 ? ValueOpcode::LoadBufferU8 :
                             bits == 16 ? ValueOpcode::LoadBufferU16 : ValueOpcode::LoadBufferU32;
        const auto store_op = bits == 8 ? ValueOpcode::StoreBufferU8 :
                              bits == 16 ? ValueOpcode::StoreBufferU16 : ValueOpcode::StoreBufferU32;
        auto& load = block->AppendNewInst(load_op,
            {Value(&handle), Value(0u), Value(&offset), Value(0u), Value(true)});
        load.SetFlags(MemoryFlags{0u, 0u});
        auto& store = block->AppendNewInst(store_op,
            {Value(&handle), Value(0u), Value(&offset), Value(0u), Value(&load), Value(true)});
        store.SetFlags(MemoryFlags{0u, 0u});
    }
    ShaderComputeInputInfo compute{};
    compute.host_subgroup_size = 32;
    CollectShaderInfo(program, {.compute = &compute});
    ShaderDeviceFeatures features{};
    features.robust_buffer_loads = robust;
    const auto spirv = ShaderRecompiler::Spirv::EmitProgram(program, {.compute = &compute}, 0, false, features);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        std::fprintf(stderr, "%s\n", message);
    });
    Require(tools.Validate(spirv), "invalid segmented SPIR-V");
    Require(program.bindings.BufferSlotCount() == 5, "segmented slots missing");
    Require(program.bindings.segmented_buffers, "logical buffer length omitted");
    std::string disassembly;
    Require(tools.Disassemble(spirv, &disassembly), "cannot inspect SPIR-V");
    Require(disassembly.find("StorageBufferArrayNonUniformIndexing") != std::string::npos,
            "no dynamic slice selection");
    Require(disassembly.find("OpULessThan") != std::string::npos,
            "logical bounds bypassed by robust-buffer optimization");
    // Check exact reconstruction around each slice boundary for every alignment prefix.
    for (uint32_t prefix = 0; prefix < 256; ++prefix) {
        const uint32_t bytes = 333 + prefix;
        for (uint32_t byte = prefix; byte < bytes; byte += bits / 8) {
            const auto element = byte / (bits / 8);
            const auto elements_per_chunk = 128 / (bits / 8);
            const auto chunk = element / elements_per_chunk;
            const auto local = element % elements_per_chunk;
            Require(chunk * 128 + local * (bits / 8) == byte / (bits / 8) * (bits / 8),
                    "upper slice address changed");
            Require(chunk < 5, "slice selection exceeds reserved descriptors");
        }
    }
    std::printf("SegmentedBufferTests: bits=%u scalar=%u robust=%u passed\n", bits, scalar, robust);
}
int main() {
    Common::InitializeThreads();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions config;
    config.printf_direction = Config::LogDirection::Silent;
    Config::Load(config);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
    ShaderInit();
    for (bool robust: {false, true}) {
        for (uint32_t bits: {8u, 16u, 32u}) Check(bits, false, robust);
        Check(32u, true, robust);
    }
}

#include "common/emulatorConfig.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include "graphics/shader/realtimeClockPolicy.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace Libs::Graphics;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "RealtimeClockTests: %s\n", why); std::abort(); }
}

void Check(uint32_t period, uint32_t wave_size) {
    using namespace ShaderRecompiler::IR;
    Program program{};
    program.stage = ShaderType::Compute;
    program.wave_size = wave_size;
    program.srt_plan_complete = true;
    program.resource_tracking_complete = true;
    program.block_storage.push_back(std::make_unique<Block>());
    auto* block = program.block_storage.back().get();
    program.blocks.push_back(block);
    for (int i = 0; i < 2; ++i) {
        auto& clock = block->AppendNewInst(ValueOpcode::ReadRealtimeClock);
        auto& low = block->AppendNewInst(ValueOpcode::CompositeExtractU64, {Value(&clock), Value(0u)});
        block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(&low)});
    }
    ShaderComputeInputInfo compute{};
    compute.host_subgroup_size = 32;
    CollectShaderInfo(program, {.compute = &compute});
    ShaderDeviceFeatures features{};
    features.device_clock_period_ps = period;
    const auto spirv = ShaderRecompiler::Spirv::EmitProgram(program, {.compute = &compute}, 0, false, features);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        std::fprintf(stderr, "%s\n", message);
    });
    Require(tools.Validate(spirv), "invalid clock SPIR-V");
    uint32_t u64_type = 0;
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16u)
        if ((spirv[p] & 0xffffu) == spv::OpTypeInt && spirv[p+2] == 64u && spirv[p+3] == 0u)
            u64_type = spirv[p+1];
    uint32_t clocks = 0, shifts = 0, capabilities = 0;
    for (size_t p = 5; p < spirv.size();) {
        const auto op = static_cast<spv::Op>(spirv[p] & 0xffffu);
        clocks += op == spv::OpReadClockKHR;
        shifts += op == spv::OpShiftRightLogical && spirv[p+1] == u64_type;
        capabilities += op == spv::OpCapability && spirv[p+1] == spv::CapabilityShaderClockKHR;
        p += spirv[p] >> 16u;
    }
    Require(clocks == (period ? 2u : 0u), "clock reads folded or duplicated across wave halves");
    Require(capabilities == (period ? 1u : 0u), "unsupported clock capability advertised");
    Require(shifts == (RealtimeClock::Shift(period) ? 2u : 0u), "incorrect host clock scaling");
    std::printf("RealtimeClockTests: period_ps=%u wave=%u reads=%u shifts=%u passed\n", period, wave_size, clocks, shifts);
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
    // Captured GCN instruction must become a dynamic IR read, not an immediate.
    // Read a start value, then poll until the low word changes. Both reads feed
    // control flow, so the complete frontend optimizer must retain them.
    const std::array<uint32_t, 7> code{0xf4940300u, 0xfa000000u,
        0xf4940380u, 0xfa000000u, 0xbf060e0cu, 0xbf85fffcu, 0xbf810000u};
    ShaderComputeInputInfo compute{};
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.input_info.compute = &compute;
    const auto translated = ShaderRecompiler::TranslateProgram(code, options);
    size_t reads = 0;
    for (const auto* block: translated.program.blocks)
        for (const auto& inst: *block)
            reads += inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::ReadRealtimeClock;
    Require(reads == 2, "captured S_MEMREALTIME lost dynamic IR reads");
    for (const auto period: {0u, 1000u, 10000u, 52083u})
        for (const auto wave: {32u, 64u}) Check(period, wave);
}

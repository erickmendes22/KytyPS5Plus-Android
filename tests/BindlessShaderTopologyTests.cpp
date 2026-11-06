#include "common/emulatorConfig.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include <spirv-tools/libspirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
using namespace Libs::Graphics;
void Require(const char* name, const char* stage, bool ok, const std::string& message) {
  if (!ok) { std::fprintf(stderr, "%s %s: %s\n", name, stage, message.c_str()); std::abort(); }
}
void ValidateSpirv(const char* name, const std::vector<uint32_t>& words) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
  });
  spvtools::ValidatorOptions options;
  options.SetAllowOffsetTextureOperand(true);
  Require(name, "SPIR-V validation", tools.Validate(words.data(), words.size(), options), "invalid module");
}
void CheckBindlessImageTopology() {
  constexpr const char *name = "BindlessImageTopology";
  constexpr uint32_t mapping_capacity = 1793u;
  using namespace ShaderRecompiler::IR;

  Program program{};
  program.stage = ShaderType::Compute;
  program.wave_size = 32;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  program.shader_info_complete = true;
  program.block_storage.push_back(std::make_unique<Block>());
  auto *block = program.block_storage.back().get();
  program.blocks.push_back(block);

  auto &key = block->AppendNewInst(ValueOpcode::LaneId);
  auto &image =
      block->AppendNewInst(ValueOpcode::GetImageResource,
                           {Value(&key), Value(0u), Value(0u), Value(0u),
                            Value(0u), Value(0u), Value(0u), Value(0u)});
  image.SetFlags<uint32_t>(0u);
  auto &sampler =
      block->AppendNewInst(ValueOpcode::GetSamplerResource,
                           {Value(0u), Value(0u), Value(0u), Value(0u)});
  sampler.SetFlags<uint32_t>(0u);
  auto &address = block->AppendNewInst(
      ValueOpcode::MakeImageAddress,
      {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u)});
  MemoryInfo memory{};
  memory.kind = ResourceKind::Image;
  memory.resource = 0;
  memory.sampler = 0;
  memory.dmask = 0xf;
  memory.image_sample_flags = ShaderRecompiler::Decoder::ImageSampleFlagLod;
  memory.image_dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  memory.image_address_components = 3;
  program.memory_info.push_back(memory);
  const MemoryFlags memory_flags{0u, 0x10f0u};
  uint64_t memory_flag_bits = 0;
  std::memcpy(&memory_flag_bits, &memory_flags, sizeof(memory_flags));
  auto &sample = block->AppendNewInst(
      ValueOpcode::ImageSampleRaw,
      {Value(&image), Value(&sampler), Value(&address), Value(true)}, memory_flag_bits);
  auto &sample_x = block->AppendNewInst(ValueOpcode::CompositeExtractU32x4,
                                        {Value(&sample), Value(0u)});
  block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(&sample_x)});

  program.descriptor_sources.resize(2);
  program.descriptor_sources[0].dword_count = 8;
  program.descriptor_sources[0].indirect_descriptor =
      DescriptorSource::IndirectDescriptor{
          .selector = DescriptorSource::IndirectDescriptor::SelectorRead{
              .source = 0, .stride = 224, .offset = 12},
          .table_source = 0, .table_stride = 32};
  program.descriptor_sources[1].dword_count = 4;

  ImageResource root{};
  root.source = 0;
  root.first_use_pc = 0x10f0u;
  root.resource_class = ImageResourceClass::Sampled;
  root.numeric_class = Prospero::TextureNumericClass::Float;
  root.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  root.read = true;
  root.indirect_root = 0;
  root.indirect_mapping_offset = 0;
	root.indirect_search_iterations = std::bit_width(mapping_capacity);
  root.indirect_resources = {0u, 1u};
  auto candidate = root;
	candidate.indirect_search_iterations = 0;
  candidate.indirect_resources.clear();
  program.info.images = {root, candidate};
  program.info.samplers.push_back({1u, 0x10f0u});
  program.info.sampled_pairs.push_back({0u, 0u, 0x10f0u});

  ShaderComputeInputInfo compute{};
  program.shader_info_complete = false;
  CollectShaderInfo(program, {.compute = &compute});
  program.bindless_images = true;
  program.info.images[0].bindless = true;
  program.info.images[0].indirect_resources.clear();
  program.info.images[1].indirect_root = ImageResource::NoIndirectImage;
  for (const bool second_bindless : {false, true}) {
    program.info.images[1].bindless = second_bindless;
    program.info.images[1].indirect_root = second_bindless ? 1u : ImageResource::NoIndirectImage;
    program.info.images[1].indirect_resources.clear();
    program.shader_info_complete = false;
    CollectShaderInfo(program, {.compute = &compute});
    const auto spirv = ShaderRecompiler::Spirv::EmitProgram(program, {.compute = &compute});
    ValidateSpirv(name, spirv);
    using Kind = DescriptorBindingKind;
    const auto kind = DescriptorBindingForImage(program.info.images[1]);
    Require(name, "local image count", kind.has_value() &&
        program.bindings.descriptor_counts[static_cast<size_t>(*kind)] ==
            (second_bindless ? 0u : 1u),
        "set 1 images consumed stage-local set 0 descriptor slots");
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string text;
    Require(name, "bindless SPIR-V", tools.Disassemble(spirv, &text) &&
        text.find("RuntimeDescriptorArray") != std::string::npos &&
        text.find("OpImageSampleExplicitLod") != std::string::npos,
        "test did not emit the GPU-indexed bindless sample");
  }
  std::printf("[host]    %-32s ok\n", name);
}

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
  ShaderInit();
  CheckBindlessImageTopology();
}

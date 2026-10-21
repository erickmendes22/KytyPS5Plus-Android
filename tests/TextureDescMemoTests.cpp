#include "graphics/host_gpu/renderer/pipeline/textureDescMemo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

using Libs::Graphics::TextureDescEntry;
using Libs::Graphics::TextureDescMemo;
using Libs::Graphics::ShaderRecompiler::IR::DescriptorValue;
using Libs::Graphics::ShaderRecompiler::IR::ImageMipMode;
using Libs::Graphics::ShaderRecompiler::IR::ImageResource;
using Libs::Graphics::ShaderRecompiler::IR::ImageResourceClass;

int g_failures = 0;

void Check(bool value, const std::string &message) {
  if (!value) {
    std::fprintf(stderr, "TextureDescMemoTests: failed: %s\n", message.c_str());
    g_failures++;
  }
}

ImageResource BaseResource() {
  ImageResource resource;
  resource.source = 3;
  resource.descriptor_index = 1;
  resource.first_use_pc = 0x40;
  resource.resource_class = ImageResourceClass::Sampled;
  resource.mip_mode = ImageMipMode::None;
  resource.read = true;
  return resource;
}

DescriptorValue BaseValue() {
  DescriptorValue value;
  value.dword_count = 8;
  for (uint32_t i = 0; i < 8; i++) {
    value.dwords[i] = 0x1000u * (i + 1u) + i;
  }
  return value;
}

TextureDescEntry Entry(uint32_t width) {
  TextureDescEntry entry;
  entry.desc.info.extent = {width, 1, 1};
  entry.shader_conversion = (width & 1u) != 0;
  return entry;
}

void TestHitAndMiss() {
  TextureDescMemo memo;
  const auto resource = BaseResource();
  const auto value = BaseValue();
  int program = 0;
  auto &slots = memo.SlotsFor(&program, 2);
  Check(slots.size() == 2, "SlotsFor did not size the program's slots");
  Check(TextureDescMemo::Find(slots[0], resource, value) == nullptr, "empty slot returned an entry");
  TextureDescMemo::Store(slots[0], resource, value, Entry(17));
  const auto *found = TextureDescMemo::Find(memo.SlotsFor(&program, 2)[0], resource, value);
  Check(found != nullptr && found->desc.info.extent.width == 17 && found->shader_conversion,
        "identical key did not return the stored entry");
  Check(TextureDescMemo::Find(slots[1], resource, value) == nullptr, "another slot shared the entry");
  int other = 0;
  Check(TextureDescMemo::Find(memo.SlotsFor(&other, 2)[0], resource, value) == nullptr,
        "another program shared the entry");
}

// Key completeness: the translation is a function of the whole ImageResource and DescriptorValue,
// so changing any one field of either must miss. A new ImageResource field changes its size;
// add a mutation for it here before updating the size check.
void TestEveryFieldIsKey() {
  static_assert(sizeof(void *) != 8 || sizeof(ImageResource) == 88,
                "ImageResource changed: add the new field to TestEveryFieldIsKey");
  const std::vector<std::pair<const char *, std::function<void(ImageResource &)>>> mutations = {
      {"source", [](ImageResource &r) { r.source++; }},
      {"descriptor_index", [](ImageResource &r) { r.descriptor_index++; }},
      {"first_use_pc", [](ImageResource &r) { r.first_use_pc += 4; }},
      {"resource_class", [](ImageResource &r) { r.resource_class = ImageResourceClass::Storage; }},
      {"numeric_class",
       [](ImageResource &r) {
         r.numeric_class = static_cast<decltype(r.numeric_class)>(static_cast<int>(r.numeric_class) + 1);
       }},
      {"dimension",
       [](ImageResource &r) {
         r.dimension = static_cast<decltype(r.dimension)>(static_cast<int>(r.dimension) + 1);
       }},
      {"mip_mode", [](ImageResource &r) { r.mip_mode = ImageMipMode::Dynamic; }},
      {"mip_count", [](ImageResource &r) { r.mip_count++; }},
      {"conversion_format",
       [](ImageResource &r) {
         r.conversion_format =
             static_cast<decltype(r.conversion_format)>(static_cast<int>(r.conversion_format) + 1);
       }},
      {"shader_swizzle", [](ImageResource &r) { r.shader_swizzle ^= 1u; }},
      {"read", [](ImageResource &r) { r.read = !r.read; }},
      {"written", [](ImageResource &r) { r.written = !r.written; }},
      {"atomic", [](ImageResource &r) { r.atomic = !r.atomic; }},
      {"atomic64", [](ImageResource &r) { r.atomic64 = !r.atomic64; }},
      {"unfiltered_access_only", [](ImageResource &r) { r.unfiltered_access_only = !r.unfiltered_access_only; }},
      {"texel_access", [](ImageResource &r) { r.texel_access = !r.texel_access; }},
      {"depth_compare", [](ImageResource &r) { r.depth_compare = !r.depth_compare; }},
      {"cube", [](ImageResource &r) { r.cube = !r.cube; }},
      {"r128", [](ImageResource &r) { r.r128 = !r.r128; }},
      {"indirect_root", [](ImageResource &r) { r.indirect_root = 0; }},
      {"indirect_mapping_offset", [](ImageResource &r) { r.indirect_mapping_offset++; }},
      {"indirect_search_iterations", [](ImageResource &r) { r.indirect_search_iterations++; }},
      {"indirect_resources", [](ImageResource &r) { r.indirect_resources.push_back(2); }},
  };
  TextureDescMemo memo;
  int program = 0;
  const auto base = BaseResource();
  const auto value = BaseValue();
  auto &slot = memo.SlotsFor(&program, 1)[0];
  TextureDescMemo::Store(slot, base, value, Entry(1));
  for (const auto &[name, mutate] : mutations) {
    auto changed = base;
    mutate(changed);
    Check(!(changed == base), std::string("mutation did not change the resource: ") + name);
    Check(TextureDescMemo::Find(slot, changed, value) == nullptr,
          std::string("changed ImageResource::") + name + " hit the memo");
  }
  for (uint32_t i = 0; i < 8; i++) {
    auto changed = value;
    changed.dwords[i] ^= 0x80000000u;
    Check(TextureDescMemo::Find(slot, base, changed) == nullptr,
          "changed descriptor dword " + std::to_string(i) + " hit the memo");
  }
  auto shorter = value;
  shorter.dword_count = 4;
  Check(TextureDescMemo::Find(slot, base, shorter) == nullptr, "changed descriptor dword_count hit the memo");
  Check(TextureDescMemo::Find(slot, base, value) != nullptr, "base key no longer hits after the misses");
  auto next = value;
  next.dwords[0] = 7;
  TextureDescMemo::Store(slot, base, next, Entry(2));
  Check(TextureDescMemo::Find(slot, base, value) == nullptr, "a replaced slot still hit its old key");
}

void TestBounded() {
  TextureDescMemo memo;
  std::vector<int> programs(TextureDescMemo::MaxPrograms + 1);
  for (auto &program : programs) {
    (void)memo.SlotsFor(&program, 1);
  }
  Check(memo.Programs() <= TextureDescMemo::MaxPrograms, "program table grew past MaxPrograms");
  Check(memo.SlotsFor(&programs.back(), 3).size() == 3, "slot list was not resized");
}

} // namespace

int main() {
  TestHitAndMiss();
  TestEveryFieldIsKey();
  TestBounded();
  if (g_failures != 0) {
    std::fprintf(stderr, "TextureDescMemoTests: %d failures\n", g_failures);
    return 1;
  }
  std::printf("TextureDescMemoTests: ok\n");
  return 0;
}

#include "graphics/host_gpu/renderer/colorTargetMemo.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Libs::Graphics::ColorTargetMemo;
using Libs::Graphics::RenderColorInfo;
namespace HW = Libs::Graphics::HW;

int g_failures = 0;

void Check(bool value, const std::string &message) {
  if (!value) {
    std::fprintf(stderr, "ColorTargetMemoTests: failed: %s\n", message.c_str());
    g_failures++;
  }
}

HW::RenderTarget BaseTarget() {
  HW::RenderTarget rt;
  rt.base.addr = 0x123400;
  rt.view.last_array_slice_index = 0;
  rt.attrib.num_samples = 0;
  rt.attrib.num_fragments = 0;
  rt.attrib2.width = 1919;
  rt.attrib2.height = 1079;
  rt.attrib3.dimension = 1;
  return rt;
}

RenderColorInfo Info(uint32_t width) {
  RenderColorInfo info;
  info.desc.info.extent = {width, 1, 1};
  info.guest_mip_level = 0;
  return info;
}

template <typename E> E Next(E value) { return static_cast<E>(static_cast<int>(value) + 1); }

void TestHitAndMiss() {
  ColorTargetMemo memo;
  const auto rt = BaseTarget();
  Check(memo.Find(0, rt, 0) == nullptr, "empty slot returned an entry");
  memo.Store(0, rt, 0, Info(1920));
  const auto *found = memo.Find(0, rt, 0);
  Check(found != nullptr && found->desc.info.extent.width == 1920, "identical key did not hit");
  Check(memo.Find(1, rt, 0) == nullptr, "another color slot shared the entry");
  Check(memo.Find(0, rt, 1) == nullptr, "another slice offset hit");
  Check(memo.Find(ColorTargetMemo::Slots, rt, 0) == nullptr, "out-of-range slot hit");
  auto other = rt;
  other.base.addr += 0x1000;
  memo.Store(0, other, 0, Info(64));
  Check(memo.Find(0, rt, 0) == nullptr, "a replaced slot still hit its old key");
}

// Key completeness: every field of every CB register struct is part of the key (defaulted
// operator==). A new field changes the struct size; add a mutation for it before updating the
// size check.
void TestEveryRegisterFieldIsKey() {
  static_assert(sizeof(void *) != 8 || sizeof(HW::RenderTarget) == 128,
                "HW::RenderTarget changed: add the new field to TestEveryRegisterFieldIsKey");
  using M = std::pair<const char *, std::function<void(HW::RenderTarget &)>>;
  const std::vector<M> mutations = {
      {"base.addr", [](HW::RenderTarget &r) { r.base.addr ^= 0x100; }},
      {"view.base_array_slice_index", [](HW::RenderTarget &r) { r.view.base_array_slice_index++; }},
      {"view.last_array_slice_index", [](HW::RenderTarget &r) { r.view.last_array_slice_index++; }},
      {"view.current_mip_level", [](HW::RenderTarget &r) { r.view.current_mip_level++; }},
      {"info.fmask_compression_enable", [](HW::RenderTarget &r) { r.info.fmask_compression_enable ^= true; }},
      {"info.fmask_data_compression_disable", [](HW::RenderTarget &r) { r.info.fmask_data_compression_disable ^= true; }},
      {"info.fmask_one_frag_mode", [](HW::RenderTarget &r) { r.info.fmask_one_frag_mode ^= true; }},
      {"info.cmask_fast_clear_enable", [](HW::RenderTarget &r) { r.info.cmask_fast_clear_enable ^= true; }},
      {"info.dcc_compression_enable", [](HW::RenderTarget &r) { r.info.dcc_compression_enable ^= true; }},
      {"info.blend_clamp", [](HW::RenderTarget &r) { r.info.blend_clamp ^= true; }},
      {"info.blend_bypass", [](HW::RenderTarget &r) { r.info.blend_bypass ^= true; }},
      {"info.round_mode", [](HW::RenderTarget &r) { r.info.round_mode ^= true; }},
      {"info.format", [](HW::RenderTarget &r) { r.info.format = Next(r.info.format); }},
      {"info.channel_type", [](HW::RenderTarget &r) { r.info.channel_type = Next(r.info.channel_type); }},
      {"info.channel_order", [](HW::RenderTarget &r) { r.info.channel_order = Next(r.info.channel_order); }},
      {"attrib.force_dest_alpha_to_one", [](HW::RenderTarget &r) { r.attrib.force_dest_alpha_to_one ^= true; }},
      {"attrib.num_samples", [](HW::RenderTarget &r) { r.attrib.num_samples++; }},
      {"attrib.num_fragments", [](HW::RenderTarget &r) { r.attrib.num_fragments++; }},
      {"attrib2.height", [](HW::RenderTarget &r) { r.attrib2.height++; }},
      {"attrib2.width", [](HW::RenderTarget &r) { r.attrib2.width++; }},
      {"attrib2.num_mip_levels", [](HW::RenderTarget &r) { r.attrib2.num_mip_levels++; }},
      {"attrib3.depth", [](HW::RenderTarget &r) { r.attrib3.depth++; }},
      {"attrib3.tile_mode", [](HW::RenderTarget &r) { r.attrib3.tile_mode = Next(r.attrib3.tile_mode); }},
      {"attrib3.dimension", [](HW::RenderTarget &r) { r.attrib3.dimension++; }},
      {"attrib3.metadata_pipe_aligned", [](HW::RenderTarget &r) { r.attrib3.metadata_pipe_aligned ^= true; }},
      {"attrib3.write_vrs_rate_hint_to_cmask", [](HW::RenderTarget &r) { r.attrib3.write_vrs_rate_hint_to_cmask ^= true; }},
      {"dcc.max_uncompressed_block_size", [](HW::RenderTarget &r) { r.dcc.max_uncompressed_block_size++; }},
      {"dcc.max_compressed_block_size", [](HW::RenderTarget &r) { r.dcc.max_compressed_block_size++; }},
      {"dcc.color_transform", [](HW::RenderTarget &r) { r.dcc.color_transform++; }},
      {"dcc.dcc_clear_key_enable", [](HW::RenderTarget &r) { r.dcc.dcc_clear_key_enable ^= true; }},
      {"dcc.overwrite_combiner_disable", [](HW::RenderTarget &r) { r.dcc.overwrite_combiner_disable ^= true; }},
      {"dcc.independent_block_size", [](HW::RenderTarget &r) { r.dcc.independent_block_size = Next(r.dcc.independent_block_size); }},
      {"dcc.data_write_on_dcc_clear_to_reg", [](HW::RenderTarget &r) { r.dcc.data_write_on_dcc_clear_to_reg ^= true; }},
      {"cmask.addr", [](HW::RenderTarget &r) { r.cmask.addr += 0x100; }},
      {"fmask.addr", [](HW::RenderTarget &r) { r.fmask.addr += 0x100; }},
      {"clear_word0.word0", [](HW::RenderTarget &r) { r.clear_word0.word0++; }},
      {"clear_word1.word1", [](HW::RenderTarget &r) { r.clear_word1.word1++; }},
      {"dcc_addr.addr", [](HW::RenderTarget &r) { r.dcc_addr.addr += 0x100; }},
  };
  ColorTargetMemo memo;
  const auto base = BaseTarget();
  memo.Store(2, base, 0, Info(7));
  for (const auto &[name, mutate] : mutations) {
    auto changed = base;
    mutate(changed);
    Check(!(changed == base), std::string("mutation did not change the registers: ") + name);
    Check(memo.Find(2, changed, 0) == nullptr, std::string("changed ") + name + " hit the memo");
  }
  Check(memo.Find(2, base, 0) != nullptr, "base key no longer hits after the misses");
}

} // namespace

int main() {
  TestHitAndMiss();
  TestEveryRegisterFieldIsKey();
  if (g_failures != 0) {
    std::fprintf(stderr, "ColorTargetMemoTests: %d failures\n", g_failures);
    return 1;
  }
  std::printf("ColorTargetMemoTests: ok\n");
  return 0;
}

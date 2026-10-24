#include "graphics/host_gpu/gpuMemoryBudget.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;
namespace {
void Check(bool ok, const char *message) {
  if (!ok) {
    std::fprintf(stderr, "ImageReadbackChunkTests: %s\n", message);
    std::abort();
  }
}
uint64_t Address(const ImageInfo &info, uint32_t level, uint32_t layer,
                 uint32_t x, uint32_t y) {
  const auto &mip = info.mip_layout[level];
  const auto &block = info.tiling.block;
  const uint64_t stride =
      info.IsTiled() ? info.tiled_slice_stride : info.linear_slice_stride;
  const auto base = mip.offset + uint64_t{layer} * stride;
  if (!info.IsTiled())
    return base + (uint64_t{y} * mip.pitch + x) * info.bytes_per_block;
  const bool tail = level >= info.first_tail_level;
  const auto bx = tail ? 0u : x / block.block_width;
  const auto by = tail ? 0u : y / block.block_height;
  const auto sx = x + mip.tail_x, sy = y + mip.tail_y;
  uint32_t local = 0, phase = 0;
  Check(TileGetBlockOffset(block, sx % block.block_width,
                           sy % block.block_height, 0, local),
        "invalid in-block coordinates");
  Check(TileGetBlockXor(block, sx / block.block_width, sy / block.block_height,
                        mip.surface_z + layer, phase),
        "invalid XOR phase");
  const uint64_t columns =
      (uint64_t{mip.pitch} + block.block_width - 1) / block.block_width;
  return base + (uint64_t{by} * columns + bx) * block.block_size +
         (local ^ phase);
}
void Verify(ImageInfo info, uint64_t capacity) {
  info.data.address = 0x100000;
  uint32_t chunks = 0;
  for (uint32_t level = 0; level < info.resources.levels; ++level) {
    const auto extent = info.MipExtent(level);
    const auto width =
        (extent.width + info.tiling.texel_width - 1) / info.tiling.texel_width;
    const auto height = (extent.height + info.tiling.texel_height - 1) /
                        info.tiling.texel_height;
    for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
      for (uint32_t row = 0; row < height;) {
        ImageInfo window;
        vk::BufferImageCopy copy;
        uint32_t rows = 0;
        Check(
            info.ReadbackChunk(level, layer, row, capacity, window, copy, rows),
            "valid layout rejected");
        Check(rows && rows <= height - row && window.data.size <= capacity &&
                  window.LinearSize() <= capacity,
              "unbounded window");
        Check(copy.imageSubresource.mipLevel == level &&
                  copy.imageSubresource.baseArrayLayer == layer &&
                  copy.imageSubresource.layerCount == 1 &&
                  copy.imageOffset.y ==
                      static_cast<int32_t>(row * info.tiling.texel_height),
              "native copy lost original subresource");
        const auto offset = window.data.address - info.data.address;
        Check(offset <= info.data.size &&
                  window.data.size <= info.data.size - offset,
              "guest range escaped image");
        for (auto x : std::array{0u, width / 2, width - 1})
          for (auto y : std::array{0u, rows / 2, rows - 1}) {
            const auto source = Address(info, level, layer, x, row + y);
            const auto local = Address(window, 0, 0, x, y);
            Check(local + info.bytes_per_block <= window.data.size &&
                      source == offset + local,
                  "changed mip, layer, row or XOR address");
          }
        row += rows;
        ++chunks;
      }
    }
  }
  Check(chunks > 1, "fixture did not exercise progression");
  ImageInfo unused;
  vk::BufferImageCopy copy;
  uint32_t rows;
  Check(!info.ReadbackChunk(info.resources.levels, 0, 0, capacity, unused, copy,
                            rows),
        "out-of-range mip accepted");
  Check(!info.ReadbackChunk(0, info.resources.layers, 0, capacity, unused, copy,
                            rows),
        "out-of-range layer accepted");
  Check(!info.ReadbackChunk(0, 0, 0, 0, unused, copy, rows),
        "zero capacity accepted");
}
ImageInfo Make(Prospero::BufferFormat format, Prospero::TileMode mode,
               uint32_t width, uint32_t height, uint32_t levels,
               uint32_t layers) {
  ImageInfo info;
  info.guest_format = format;
  info.extent = vk::Extent3D{width, height, 1};
  info.tile_mode = mode;
  info.resources = {levels, layers};
  info.UpdateSize();
  return info;
}
} // namespace
int main() {
  using namespace GpuBudget;
  for (auto mode :
       {Prospero::TileMode::kStandard256B, Prospero::TileMode::kStandard4KB,
        Prospero::TileMode::kStandard64KB, Prospero::TileMode::kPrt,
        Prospero::TileMode::kRenderTarget}) {
    for (auto format :
         {Prospero::BufferFormat::k8UNorm, Prospero::BufferFormat::k16UNorm,
          Prospero::BufferFormat::k32Float, Prospero::BufferFormat::k32_32Float,
          Prospero::BufferFormat::k32_32_32_32Float,
          Prospero::BufferFormat::kBc1UNorm}) {
      TileTextureBlockLayout block;
      if (!TileGetTextureBlockLayout(format, mode, false, block))
        continue;
      auto info = Make(format, mode, 1031, 521, 10, 3);
      const uint64_t columns =
          (uint64_t{info.mip_layout[0].pitch} + block.block.block_width - 1) /
          block.block.block_width;
      Verify(info, columns * block.block.block_size * 2);
    }
  }
  Verify(Make(Prospero::BufferFormat::k32Float,
              Prospero::TileMode::kStandard64KB, 8192, 8192, 1, 1),
         kWrittenChunkBytes);
  Verify(Make(Prospero::BufferFormat::k32Float, Prospero::TileMode::kLinear,
              8192, 8192, 1, 1),
         kWrittenChunkBytes);
  Verify(Make(Prospero::BufferFormat::k32Float, Prospero::TileMode::kLinear,
              1031, 521, 10, 3),
         64 * 1024);
  std::puts("ImageReadbackChunkTests: ok");
}

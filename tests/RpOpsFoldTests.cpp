#include "graphics/host_gpu/renderer/image/rpFold.h"

#include <cstdio>
#include <string>
#include <vector>

// KYTY_RP_OPS clear fold: a pending clear is absorbed only by a full-area, same-format colour
// pass; every other access records it first, in guest order. SimImage mirrors how Image
// (GetBarriers -> FlushFoldedClear), TextureCache::ClearImage and
// RenderExecutor::AcquireRenderTargets drive the shared RpFold rules.

namespace {

namespace Rp = Libs::Graphics::RpFold;

int g_failures = 0;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "RpOpsFoldTests: failed: %s\n", message);
    g_failures++;
  }
}

constexpr Rp::Shape kPlain {.color_aspect = true, .full_image = true};

struct SimImage {
  Rp::Tracker<int> fold;
  std::vector<std::string> log; // the command stream as recorded

  void Clear(int value, Rp::Shape shape = kPlain) {
    if (Rp::Eligible(shape)) {
      fold.Fold(value);
    } else {
      log.push_back("clear " + std::to_string(value));
    }
  }
  // Any layout change other than the absorbing pass's attachment acquisition.
  void Use(const char *what) {
    if (fold.NeedsFlush(false)) log.push_back("clear " + std::to_string(fold.TakeForFlush()));
    log.push_back(what);
  }
  void Pass(Rp::PassUse use) {
    // The attachment acquisition keeps the fold pending (FoldScope).
    Check(!fold.NeedsFlush(true), "attachment acquisition flushed the fold");
    int value = 0;
    if (fold.TryAbsorb(use, value)) {
      log.push_back("pass clear " + std::to_string(value));
      return;
    }
    if (fold.Pending()) log.push_back("clear " + std::to_string(fold.TakeForFlush()));
    log.push_back("pass load");
  }
};

constexpr Rp::PassUse kFull {.pass_width = 1920, .pass_height = 1080, .image_width = 1920,
                             .image_height = 1080, .same_format = true};

bool Is(const SimImage &image, std::vector<std::string> expected) { return image.log == expected; }

} // namespace

int main() {
  // Eligibility.
  static_assert(Rp::Eligible(kPlain));
  static_assert(!Rp::Eligible({.color_aspect = false, .full_image = true}));
  static_assert(!Rp::Eligible({.color_aspect = true, .full_image = false}));
  static_assert(!Rp::Eligible({.color_aspect = true, .full_image = true, .levels = 2}));
  static_assert(!Rp::Eligible({.color_aspect = true, .full_image = true, .layers = 6}));
  static_assert(!Rp::Eligible({.color_aspect = true, .full_image = true, .samples = 4}));
  static_assert(!Rp::Eligible({.color_aspect = true, .full_image = true, .volume = true}));
  static_assert(Rp::Absorbs(kFull));

  {  // Absorbed by a full-area, same-format pass; later passes load.
    SimImage image;
    image.Clear(7);
    image.Pass(kFull);
    image.Pass(kFull);
    Check(Is(image, {"pass clear 7", "pass load"}), "full pass absorbs once");
  }
  {  // The last of two clears wins.
    SimImage image;
    image.Clear(1);
    image.Clear(2);
    image.Pass(kFull);
    Check(Is(image, {"pass clear 2"}), "second clear replaces the first");
  }
  for (const char *use : {"sample", "storage", "copy-src", "copy-dst", "upload", "download"}) {
    SimImage image;
    image.Clear(7);
    image.Use(use);
    image.Pass(kFull);
    Check(Is(image, {"clear 7", use, "pass load"}), use);
  }
  {  // Partial-area pass.
    SimImage image;
    image.Clear(7);
    auto partial = kFull;
    partial.pass_width = 960;
    image.Pass(partial);
    Check(Is(image, {"clear 7", "pass load"}), "partial area flushes first");
  }
  {  // A view in another format.
    SimImage image;
    image.Clear(7);
    auto other = kFull;
    other.same_format = false;
    image.Pass(other);
    Check(Is(image, {"clear 7", "pass load"}), "different format flushes first");
  }
  {  // A non-zero view level or several layers.
    SimImage image;
    image.Clear(7);
    auto mip = kFull;
    mip.view_base_level = 1;
    image.Pass(mip);
    auto layers = kFull;
    layers.pass_layers = 2;
    image.Clear(8);
    image.Pass(layers);
    Check(Is(image, {"clear 7", "pass load", "clear 8", "pass load"}), "level/layers flush first");
  }
  for (const auto shape : {Rp::Shape {.color_aspect = true, .full_image = true, .levels = 3},
                           Rp::Shape {.color_aspect = true, .full_image = true, .layers = 2},
                           Rp::Shape {.color_aspect = true, .full_image = true, .samples = 2},
                           Rp::Shape {.color_aspect = true, .full_image = true, .volume = true},
                           Rp::Shape {.color_aspect = false, .full_image = true}}) {
    SimImage image;
    image.Clear(7, shape);
    image.Pass(kFull);
    Check(Is(image, {"clear 7", "pass load"}), "ineligible image is cleared at once");
  }
  {  // Ordering across interleaved uses: the flush lands right before the first other use.
    SimImage image;
    image.Clear(5);
    image.Use("sample");
    image.Use("sample");
    image.Clear(6);
    image.Pass(kFull);
    Check(Is(image, {"clear 5", "sample", "sample", "pass clear 6"}), "guest order kept");
  }
  if (g_failures != 0) return 1;
  std::printf("RpOpsFoldTests: ok\n");
  return 0;
}

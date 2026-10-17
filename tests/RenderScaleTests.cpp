#include "graphics/host_gpu/renderer/image/renderScale.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;
using Libs::Graphics::Prospero::ImageType;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "RenderScaleTests: failed: %s\n", text);
		std::abort();
	}
}

void TestActive() {
	Check(!RenderScale::Active(1.0f), "1.0 is off");
	Check(!RenderScale::Active(1.0004f), "1.0004 is off");
	Check(!RenderScale::Active(0.0f), "0 is off");
	Check(RenderScale::Active(0.5f), "0.5 is on");
	Check(RenderScale::Active(0.75f), "0.75 is on");
	Check(RenderScale::Active(1.5f), "1.5 is on");
}

void TestScaleDimension() {
	Check(RenderScale::ScaleDimension(1920, 0.5f) == 960, "1920 * 0.5");
	Check(RenderScale::ScaleDimension(1080, 0.5f) == 540, "1080 * 0.5");
	Check(RenderScale::ScaleDimension(1920, 0.75f) == 1440, "1920 * 0.75");
	Check(RenderScale::ScaleDimension(1080, 0.75f) == 810, "1080 * 0.75");
	Check(RenderScale::ScaleDimension(1, 0.25f) == 1, "never below one texel");
	Check(RenderScale::ScaleDimension(1087, 0.5f) == 544, "odd sizes round to nearest");
	const auto extent = RenderScale::ScaleExtent({3840, 2160, 1}, 0.5f);
	Check(extent.width == 1920 && extent.height == 1080 && extent.depth == 1,
	      "extent scales width and height only");
}

void TestFrameSized() {
	Check(RenderScale::FrameSized(1920, 1080), "1080p is frame sized");
	Check(RenderScale::FrameSized(1920, 1088), "padded 1080p is frame sized");
	Check(RenderScale::FrameSized(3840, 2160), "4K is frame sized");
	Check(RenderScale::FrameSized(1280, 720), "720p is frame sized");
	Check(RenderScale::FrameSized(2560, 1080), "21:9 is frame sized");
	Check(!RenderScale::FrameSized(2048, 2048), "square shadow maps are not");
	Check(!RenderScale::FrameSized(4096, 1024), "wide atlases are not");
	Check(!RenderScale::FrameSized(960, 540), "half-res buffers are not");
	Check(!RenderScale::FrameSized(1024, 512), "too short");
}

void TestGeometryEligible() {
	Check(RenderScale::GeometryEligible(ImageType::kColor2D, {1920, 1080, 1}, 1, 1, false),
	      "single-mip 2D color target");
	Check(!RenderScale::GeometryEligible(ImageType::kColor2D, {1920, 1080, 1}, 4, 1, false),
	      "mip chains stay 1:1");
	Check(!RenderScale::GeometryEligible(ImageType::kColor2D, {1920, 1080, 1}, 1, 4, false),
	      "MSAA stays 1:1");
	Check(!RenderScale::GeometryEligible(ImageType::kColor3D, {1920, 1080, 16}, 1, 1, false),
	      "volumes stay 1:1");
	Check(!RenderScale::GeometryEligible(ImageType::kColor2D, {1920, 1080, 1}, 1, 1, true),
	      "block-compressed images stay 1:1");
	Check(!RenderScale::GeometryEligible(ImageType::kColor2D, {256, 256, 1}, 1, 1, false),
	      "small images stay 1:1");
}

void TestScaleRect() {
	const RenderScale::Rect full {0, 0, 1920, 1080};
	Check(RenderScale::ScaleRect(full, 0.5f, 960, 540) == RenderScale::Rect {0, 0, 960, 540},
	      "full-frame scissor");
	Check(RenderScale::ScaleRect({3, 5, 1917, 1075}, 0.5f, 960, 540) ==
	          RenderScale::Rect {1, 2, 959, 538},
	      "inset scissor grows outward to whole pixels");
	Check(RenderScale::ScaleRect({0, 0, 16384, 16384}, 0.5f, 960, 540) ==
	          RenderScale::Rect {0, 0, 960, 540},
	      "oversized scissor clamps to the host extent");
	Check(RenderScale::ScaleRect({100, 100, 100, 300}, 0.5f, 960, 540) ==
	          RenderScale::Rect {0, 0, 0, 0},
	      "empty scissor stays empty");
	Check(RenderScale::ScaleRect({0, 0, 1920, 1080}, 0.75f, 1440, 810) ==
	          RenderScale::Rect {0, 0, 1440, 810},
	      "three-quarter scissor");
}

void TestScaleViewport() {
	const vk::Viewport guest {10.0f, 20.0f, 1900.0f, 1040.0f, 0.0f, 1.0f};
	const auto         host = RenderScale::ScaleViewport(guest, 0.5f);
	Check(host.x == 5.0f && host.y == 10.0f && host.width == 950.0f && host.height == 520.0f,
	      "viewport origin and size scale");
	Check(host.minDepth == 0.0f && host.maxDepth == 1.0f, "depth range is unchanged");
}

} // namespace

int main() {
	TestActive();
	TestScaleDimension();
	TestFrameSized();
	TestGeometryEligible();
	TestScaleRect();
	TestScaleViewport();
	std::puts("RenderScaleTests: ok");
	return 0;
}

#include "graphics/host_gpu/renderer/demonsDominantGeometry.h"
#include "graphics/host_gpu/renderer/demonsQueueProbe.h"
#include "graphics/host_gpu/renderer/demonsAdaptiveCapture.h"
#include "graphics/host_gpu/renderer/demonsDepthEqualPolicy.h"

#include <cassert>
#include <fstream>
#include <vector>

using namespace Libs::Graphics::DemonsQueueProbe;

using namespace Libs::Graphics::DemonsDepthEqualPolicy;
static_assert(Select(0, SuspectScene0, DepthAddress, true, false, true, 2u) ==
              Decision::Guest);
static_assert(Select(0, SuspectScene1, DepthAddress, true, false, true, 2u) ==
              Decision::Guest);
static_assert(Select(0, 0xafd9334e8581cda2ull, DepthAddress, true, false, true, 2u) ==
              Decision::Guest);
static_assert(Select(1, SuspectScene0, DepthAddress, true, false, true, 2u) ==
              Decision::LessOrEqual);
static_assert(Select(2, SuspectScene1, DepthAddress, true, false, true, 2u) ==
              Decision::GreaterOrEqual);
static_assert(Select(3, SuspectScene0, DepthAddress, true, false, true, 2u) ==
              Decision::Guest);
static_assert(Select(0, SuspectScene0, DepthAddress, true, true, true, 2u) ==
              Decision::Guest);
static_assert(Select(0, SuspectScene0, DepthAddress, true, false, false, 2u) ==
              Decision::Guest);
static_assert(Select(0, SuspectScene0, DepthAddress + 4, true, false, true, 2u) ==
              Decision::Guest);
static_assert(Select(0, SuspectScene0, DepthAddress, true, false, true, 6u) ==
              Decision::Guest);
static_assert(GBufferShaders.size() == 6);
static_assert(GBufferShaders[3] == 0xcfc1d9193e7f9e53ull);
static_assert(GBufferShaders[4] == 0xc92deb23fc611b10ull);

static std::vector<uint32_t> ReadWords(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	const auto size = file.tellg();
	assert(size > 0 && size % sizeof(uint32_t) == 0);
	std::vector<uint32_t> words(size / sizeof(uint32_t));
	file.seekg(0);
	assert(file.read(reinterpret_cast<char*>(words.data()), size));
	return words;
}

int main(int argc, char** argv) {
	// The same transition must arm a capture irrespective of how long
	// the introduction or loading screens took.
	for (const int32_t boot_offset : {0, 367, 1982}) {
		Libs::Graphics::DemonsAdaptiveCapture::Gate detector;
		assert(detector.ShouldSample(boot_offset));
		auto ignored = detector.Observe(boot_offset, 0.001, 0, 0x281a80000ull);
		assert(!ignored.triggered && detector.ShouldSample(boot_offset + 24));
		detector.MarkGeometry(boot_offset + 24, 5);
		detector.Observe(boot_offset + 24, 0.25, 14, 0x281a80000ull);
		assert(detector.BrightSceneActive(boot_offset + 25));
		assert(detector.BrightBaselineDue(boot_offset + 25));
		assert(detector.BrightBaselineDue(boot_offset + 33));
		assert(!detector.BrightBaselineDue(boot_offset + 49));
		for (int i = 1; i <= 3; ++i) {
			assert(detector.ShouldSample(boot_offset + (i + 1) * 24));
			const auto event = detector.Observe(
			    boot_offset + (i + 1) * 24, 0.001, 0, 0x281a80000ull);
			if (i == 1) assert(detector.DarkOnsetDue(boot_offset + 49));
			if (i > 1) assert(!detector.DarkOnsetDue(boot_offset + (i + 1) * 24 + 1));
			assert(event.triggered == (i == 3));
		}
		assert(detector.CaptureDue(boot_offset + 100));
		detector.NoteCaptured(boot_offset + 100);
		assert(detector.CapturedFrame(boot_offset + 100));
		assert(!detector.CapturedFrame(boot_offset + 101));
		assert(detector.CaptureDue(boot_offset + 100)); // same-frame writes
		assert(!detector.CaptureDue(boot_offset + 120));
		assert(detector.CaptureDue(boot_offset + 124));
		detector.NoteCaptured(boot_offset + 124);
		assert(detector.CaptureDue(boot_offset + 148));
		detector.NoteCaptured(boot_offset + 148);
		assert(!detector.CaptureDue(boot_offset + 172)); // three-frame budget
		// Constant darkness must never open a fresh capture just because
		// 180 frames passed. This happened three times in the 717 ZIP.
		for (int i = 1; i <= 10; ++i) {
			const auto state = detector.Observe(
			    boot_offset + 148 + i * 24, 0.000335, 0, 0x281a80000ull);
			assert(!state.triggered);
		}
		// Real recovery must permit another independently observed transition.
		detector.MarkGeometry(boot_offset + 412, 5);
		detector.Observe(boot_offset + 412, 0.08, 14, 0x281a80000ull);
		detector.Observe(boot_offset + 436, 0.09, 15, 0x281a80000ull);
		for (int i = 1; i <= 3; ++i) {
			const auto event = detector.Observe(
			    boot_offset + 436 + i * 24, 0.0002, 0, 0x281a80000ull);
			assert(event.triggered == (i == 3));
		}
	}
	// A natural fade to black cannot trigger a late capture without new
	// deferred-geometry evidence. A sustained late candidate has its own
	// three-capture budget independent of the first fade.
	{
		using Libs::Graphics::DemonsAdaptiveCapture::Gate;
		Gate black;
		black.MarkGeometry(100, 5);
		black.Observe(100, 0.11, 64, 0x281a80000ull);
		for (int f = 124; f <= 508; f += 24) {
			black.Observe(f, 0.0001, 0, 0x281a80000ull);
			if (f == 292) assert(!black.PostTransitionDarkDue(f));
		}
		assert(!black.PostTransitionDarkDue(508));
		assert(!black.CaptureDue(508));
		// Even if the early geometry remains in the 300-frame general
		// gate, it must not count as new activity after the dark latch.
		assert(!black.PostTransitionDarkDue(220));

		Gate late;
		late.MarkGeometry(100, 5);
		late.Observe(100, 0.11, 64, 0x281a80000ull);
		for (int f = 124; f <= 532; f += 24) {
			late.MarkGeometry(f, 5);
			auto observation = late.Observe(f, 0.0062383, 2, 0x281a80000ull);
			if (f == 172) assert(observation.triggered);
		}
		assert(late.PostTransitionDarkDue(532));
		assert(late.CaptureDue(533));
		late.NoteCaptured(533);
		assert(late.CapturedFrame(533));
		assert(!late.CaptureDue(550));
		assert(!late.PostTransitionDarkDue(557)); // sample is stale
	}
	// 767 incorrectly exhausted its one-shot ray producer capture at frame
	// 206, long before gameplay turned dark. The independent producer gate
	// must never capture early, and must allow three distinct dark updates
	// even after the onset was already sampled.
	{
		using Libs::Graphics::DemonsAdaptiveCapture::RayProducerCaptureGate;
		RayProducerCaptureGate producer;
		producer.ObserveFrame(206);
		assert(!producer.Due(206, false, false));
		producer.ObserveFrame(307);
		assert(producer.Due(307, true, false));
		producer.NoteCaptured(307, true, false);
		assert(!producer.Due(308, true, false));
		assert(!producer.FollowupDue(330));
		assert(producer.FollowupDue(335));
		producer.NoteFollowup(335);
		assert(!producer.FollowupDue(340));
		assert(producer.FollowupDue(359));
		producer.NoteFollowup(359);
		assert(producer.FollowupDue(383));
		producer.NoteFollowup(383);
		assert(!producer.FollowupDue(407));
		for (const int frame : {499, 523, 547}) {
			producer.ObserveFrame(frame);
			assert(producer.Due(frame, false, true));
			producer.NoteCaptured(frame, false, true);
			assert(!producer.Due(frame + 1, false, true));
		}
		assert(!producer.Due(600, false, true));
		// Rebooting the guest within the same process re-arms a new session.
		producer.ObserveFrame(1);
		assert(producer.Due(2, true, false));
		assert(producer.Due(3, false, true));
	}
	// Replay the full 762 real HDR observations. It ended at 553, so the
	// original 192-frame / eight-sample gate could never fire. Verify
	// the updated gate captures distinct material/atlas pairs BEFORE then,
	// including the frame-527 HUD flicker (0.0105114 / 3 lit pixels).
	{
		using Libs::Graphics::DemonsAdaptiveCapture::Gate;
		Gate run762;
		constexpr uint64_t scene = 0x281a80000ull;
		struct Sample { int32_t frame; double level; uint32_t visible; };
		for (const Sample sample : {
			Sample{215, 0.1007097, 64}, Sample{239, 0.1036064, 64},
			Sample{263, 0.0369873, 64}, Sample{287, 0.1051085, 64},
			Sample{311, 0.0512764, 64}, Sample{335, 0.0000000, 0},
			Sample{359, 0.0062383, 2}, Sample{383, 0.0062383, 2},
			Sample{407, 0.0062383, 2}, Sample{431, 0.0062383, 2},
			Sample{455, 0.0062383, 2}, Sample{479, 0.0062383, 2},
			Sample{503, 0.0062383, 2}, Sample{527, 0.0105114, 3},
			Sample{551, 0.0062383, 2}}) {
			assert(run762.ShouldSample(sample.frame));
			// This reproduces the actual queue-33 readback cadence: the
			// exported 762 log verified colored geometry at 241 and 481,
			// NOT at every HDR sample.
			if (sample.frame == 215) run762.MarkGeometry(215, 5);
			if (sample.frame == 263) run762.MarkGeometry(241, 5);
			if (sample.frame == 503) run762.MarkGeometry(481, 5);
			const auto state = run762.Observe(sample.frame, sample.level,
			                                sample.visible, scene);
			if (sample.frame == 383) assert(state.triggered);
			if (sample.frame == 479) assert(!run762.PostTransitionDarkDue(480));
			if (sample.frame == 503 || sample.frame == 527 || sample.frame == 551) {
				assert(state.reason == std::string("post_transition_dark_candidate"));
				assert(run762.PostTransitionDarkDue(sample.frame + 1));
				// The scene and light queues can be captured before
				// the coefficient/atlas pair completes in this frame.
				assert(!run762.CapturedFrame(sample.frame + 1));
				assert(run762.CaptureDue(sample.frame + 1));
				run762.NoteCaptured(sample.frame + 1);
				assert(run762.CapturedFrame(sample.frame + 1));
			}
		}
		assert(!run762.CaptureDue(553)); // bounded at three paired frames
	}
	// Reproduce the *observed* 718 ordering: the G-buffer shows changing
	// colored geometry before the lighting queue is sampled; the compositor
	// sees a bright scene, then three progressively dark observations.
	for (const int32_t base : {0, 351, 2400}) {
		Libs::Graphics::DemonsAdaptiveCapture::Gate detector;
		detector.MarkGeometry(base + 80, 1439860); // a real changed material attachment
		for (int i = 0; i < 4; ++i) {
			const int frame = base + 80 + 24 * i;
			assert(detector.ShouldSample(frame));
			const auto observation = detector.Observe(
			    frame, 0.101, 64, 0x281a80000ull);
			assert(!observation.triggered);
		}
		for (int i = 1; i <= 3; ++i) {
			const auto observed = detector.Observe(
			    base + 176 + i * 24, 0.0062383, 2, 0x281a80000ull);
			assert(observed.triggered == (i == 3));
		}
		assert(detector.CaptureDue(base + 300));
		detector.NoteCaptured(base + 300);
		assert(detector.CapturedFrame(base + 300));
	}
	static_assert(DefaultPixels(1600, 900)[4] == std::pair{1200u, 675u});
	constexpr auto coverage = MaterialCoveragePixels(1600, 900, 0);
	static_assert(coverage.size() == 96 && coverage.front() == std::pair{33u, 28u});
	static_assert(coverage.back() == std::pair{1500u, 815u});
	static_assert(MaterialCoveragePixels(1600, 900, 8).back() == std::pair{1566u, 871u});
	static_assert(PixelInScissor({12, 9}, 10, 9, 3, 1));
	static_assert(!PixelInScissor({13, 9}, 10, 9, 3, 1));
	static_assert(!PixelInScissor({9, 9}, 10, 9, 3, 1));
	static_assert(SampleBlock(5, 12).offset == 4 && SampleBlock(5, 12).extent == 4);
	static_assert(SampleBlock(8, 10).offset == 8 && SampleBlock(8, 10).extent == 2);
	static_assert(SampleBlock(0, 1).offset == 0 && SampleBlock(0, 1).extent == 1);
	static_assert(PackedRgb10IsBlack(0xc0000000u));
	static_assert(!PackedRgb10IsBlack(0xc0000001u));
	static_assert(!PackedRgb10IsBlack(0x00100000u));
	static_assert(MaterialTracePending(891, 892));
	static_assert(MaterialTracePending(891, 903));
	static_assert(!MaterialTracePending(891, 904) && !MaterialTracePending(891, 890));
	std::vector<uint32_t> albedo(17 * 9, 0xff000000u);
	albedo[3 * 17 + 8] = 0xff112233u;
	albedo[8 * 17 + 16] = 0xff223344u;
	constexpr std::array<uint32_t, 4> colored_tiles {1, 5, 1, 100};
	Pixels covered_albedo {};
	assert(ColoredTilePixels(albedo, colored_tiles, 17, 9, covered_albedo) == 2);
	assert((covered_albedo[0] == std::pair{8u, 3u}));
	assert((covered_albedo[1] == std::pair{16u, 8u}));
	assert(ColoredTilePixels(albedo, std::array<uint32_t, 1>{0}, 17, 9, covered_albedo) == 0);
	static_assert(PostTracePending(961, 963) && !PostTracePending(961, 964));
	static_assert(!PostTracePending(-1, 0));
	static_assert(!TracePostPass(24, 0, false, false));
	static_assert(TracePostPass(24, 0, true, false));
	static_assert(!TracePostPass(24, 8, true, false));
	static_assert(TracePostPass(32, 8, false, true));
	Capture scene {961, Shaders[1], 1600, 900};
	assert(scene.TracesImage(HdrAddress) && scene.TracesImage(SceneAddress));
	assert(!scene.TracesImage(0) && !scene.TracesImage(0x123000));
	scene.TrackIntermediate(0x123000, 799, 900);
	assert(!scene.TracesImage(0x123000));
	scene.TrackIntermediate(0x123000, 1920, 1080);
	assert(scene.TracesImage(0x123000));
	std::vector<uint32_t> prefix(65);
	std::vector<uint32_t> tiles(45200);
	prefix[1] = 22089;
	prefix[33] = 22600;
	prefix[34] = 45200;
	tiles[0] = 8;
	tiles[22088] = 22599;
	tiles[45199] = 22599;

	if (argc == 3) {
		prefix = ReadWords(argv[1]);
		tiles = ReadWords(argv[2]);
	}
	const auto queue0 = SelectTiles(prefix, tiles, 0);
	const auto queue33 = SelectTiles(prefix, tiles, 33);
	assert(queue0.valid && queue0.first == 0 && queue0.end == 22089);
	assert(queue33.valid && queue33.first == 22600 && queue33.end == 45200);
	Pixels pixels {};
	assert(TilePixels(queue0, 1600, 900, pixels));
	assert((pixels.front() == std::pair {68u, 4u}));
	assert((pixels.back() == std::pair {1596u, 899u}));
	assert(TilePixels(queue33, 1600, 900, pixels));
	assert((pixels.front() == std::pair {4u, 4u}));
	assert((pixels.back() == std::pair {1596u, 899u}));
	if (argc == 3) {
		assert((queue0.ids == std::array {8u, 6858u, 12280u, 15556u, 22599u}));
		assert((queue33.ids == std::array {0u, 6809u, 10443u, 18925u, 22599u}));
	}
	assert(!TilePixels(queue33, 1600, 899 - 4, pixels));
	assert(!SelectTiles(prefix, std::span(tiles).first(45199), 33).valid);
	assert(!SelectTiles(prefix, tiles, UINT32_MAX).valid);
	assert(!SelectTiles(prefix, tiles, 2).valid);
}

namespace {
namespace D = Libs::Graphics::DemonsDominantGeometry;
static_assert(D::Watched(D::MaterialPs));
static_assert(D::SuppressDepthWrite(true,D::OccluderVs,D::DepthPs,D::DepthAddress,false,true,true,7296,1,6));
static_assert(D::SuppressDepthWrite(true,D::OccluderVs,D::DepthPs,D::DepthAddress,false,true,true,192,1,6));
static_assert(!D::SuppressDepthWrite(false,D::OccluderVs,D::DepthPs,D::DepthAddress,false,true,true,7296,1,6));
static_assert(!D::SuppressDepthWrite(true,D::DominantDepthVs,D::DepthPs,D::DepthAddress,false,true,true,7296,1,6));
static_assert(!D::SuppressDepthWrite(true,D::OccluderVs,D::DepthPs,D::DepthAddress,true,true,true,7296,1,6));
static_assert(!D::SuppressDepthWrite(true,D::OccluderVs,D::DepthPs,D::DepthAddress,false,false,true,7296,1,6));
static_assert(!D::SuppressDepthWrite(true,D::OccluderVs,D::DepthPs,D::DepthAddress,false,true,true,33,1,6));
}

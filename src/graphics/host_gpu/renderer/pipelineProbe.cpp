#include "graphics/host_gpu/renderer/pipelineProbe.h"

#include "common/perfWaits.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/gpuPipeline.h"

namespace Libs::Graphics::PipelineProbe {

namespace PS = Common::PerfStats;

namespace {

Model& GpuModel() {
	static Model model;
	return model;
}


bool Active() {
	// The model is the front's (the GPU thread's); KYTY_GPU_PIPELINE's record thread does not feed it.
	return PS::Enabled() && GuestGpu::IsGpuThread() && !GpuPipeline::OnBack();
}

constexpr PS::Counter DrainCounter(Drain cause) {
	switch (cause) {
		case Drain::SrtWrite: return PS::Counter::PipeDrainSrtWrite;
		case Drain::SrtDma: return PS::Counter::PipeDrainSrtDma;
		case Drain::SrtDirty: return PS::Counter::PipeDrainSrtDirty;
		case Drain::SyncCommand: return PS::Counter::PipeDrainSync;
		case Drain::IndirectCpu: return PS::Counter::PipeDrainIndirect;
		default: return PS::Counter::PipeDrainSync;
	}
}

} // namespace

void BeginItem() {
	if (!Active()) return;
	GpuModel().BeginItem();
	PS::Count(PS::Counter::PipeItems);
}

void MarkDmaWriter() {
	if (!Active()) return;
	GpuModel().MarkDmaWriter();
	PS::Count(PS::Counter::PipeDmaItems);
}

void AddResourceBytes(size_t bytes) {
	if (!Active()) return;
	PS::Count(PS::Counter::PipeResourceBytes, bytes);
}

void OnGpuWrite(uint64_t address, uint64_t size) {
	if (!Active()) return;
	GpuModel().OnGpuWrite(address, size);
}

void OnSrtRead(uint64_t address, uint64_t size, bool gpu_dirty) {
	if (!Active()) return;
	PS::Count(PS::Counter::PipeSrtReads);
	const auto cause = GpuModel().OnSrtRead(address, size, gpu_dirty);
	if (cause != Drain::Count) {
		PS::Count(DrainCounter(cause));
	}
}

void OnDrain(Drain cause) {
	if (!Active()) return;
	GpuModel().OnDrain();
	PS::Count(DrainCounter(cause));
}

void OnWaitRegMemBlocked() {
	if (!Active()) return;
	PS::Count(PS::Counter::PipeWaitRegMemBlocked);
}

void CountRegisterBlockAccess() {
	if (PS::Enabled()) {
		PS::Count(PS::Counter::PipeRecordRegAccess);
	}
}

} // namespace Libs::Graphics::PipelineProbe

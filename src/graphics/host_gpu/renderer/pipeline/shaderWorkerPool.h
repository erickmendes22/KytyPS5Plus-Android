#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWORKERPOOL_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWORKERPOOL_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {

// Runs shader translations and pipeline compilations off the GPU thread (--async-shaders).
//
// A Job owns every input it reads and every result it produces until the submitting thread
// observes Done() (acquire) and takes the results; the pool never touches a job after Run()
// returns. With zero threads Submit() runs the job inline, so callers have one code path for
// the synchronous and the asynchronous policy.
class ShaderWorkerPool {
public:
	class Job {
	public:
		virtual ~Job() = default;
		virtual void Run() = 0;

		// True once Run() has returned on some thread. The results written by Run() are visible
		// to a thread that observed true.
		[[nodiscard]] bool Done() const { return m_done.load(std::memory_order_acquire); }
		// Blocks until Done(). Only for jobs the game needs before it can continue (compute).
		void Wait();

	private:
		friend class ShaderWorkerPool;
		void Execute();

		// Set under the pool's queue lock by whoever takes the job (a worker, Submit on a
		// synchronous pool, or RunNow), so it runs exactly once.
		std::atomic<bool>       m_started {false};
		std::atomic<bool>       m_done {false};
		std::mutex              m_mutex;
		std::condition_variable m_finished;
	};

	// threads == 0: synchronous; every Submit() runs the job before returning.
	explicit ShaderWorkerPool(uint32_t threads);
	~ShaderWorkerPool();
	ShaderWorkerPool(const ShaderWorkerPool&)            = delete;
	ShaderWorkerPool& operator=(const ShaderWorkerPool&) = delete;

	// Queues the job (or runs it inline for a synchronous pool). The job must not be Done().
	void Submit(std::shared_ptr<Job> job);
	// Runs the job on the calling thread now (queued, or never submitted), unless a worker
	// already started it; then waits for that worker. Used when a result is needed before the
	// caller can continue.
	void RunNow(const std::shared_ptr<Job>& job);

	[[nodiscard]] bool     Async() const { return !m_threads.empty(); }
	[[nodiscard]] uint32_t ThreadCount() const { return static_cast<uint32_t>(m_threads.size()); }
	[[nodiscard]] uint32_t Pending() const;

	// True on a pool worker thread (perf accounting attributes the work to the worker).
	[[nodiscard]] static bool OnWorkerThread();
	// Worker count for a device with the given hardware thread count.
	[[nodiscard]] static uint32_t DefaultThreadCount(uint32_t hardware_threads);

private:
	void WorkerMain(uint32_t index);

	std::vector<std::thread>         m_threads;
	mutable std::mutex               m_mutex;
	std::condition_variable          m_wake;
	std::deque<std::shared_ptr<Job>> m_queue;
	bool                             m_stop = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERWORKERPOOL_H_

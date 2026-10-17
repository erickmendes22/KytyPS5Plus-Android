#include "graphics/host_gpu/renderer/pipeline/shaderWorkerPool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "ShaderWorkerPoolTests: failed: %s\n", message);
    std::abort();
  }
}

using Libs::Graphics::ShaderWorkerPool;

// Records which thread ran it and produces a result the submitter reads after Done().
class CountingJob : public ShaderWorkerPool::Job {
public:
  explicit CountingJob(std::atomic<uint32_t> &runs, uint32_t value)
      : m_runs(runs), m_value(value) {}

  void Run() override {
    on_worker = ShaderWorkerPool::OnWorkerThread();
    thread = std::this_thread::get_id();
    result = m_value * 2u;
    m_runs.fetch_add(1, std::memory_order_relaxed);
  }

  bool on_worker = false;
  std::thread::id thread;
  uint32_t result = 0;

private:
  std::atomic<uint32_t> &m_runs;
  uint32_t m_value;
};

// Holds until released, to keep a worker busy.
class BlockingJob : public ShaderWorkerPool::Job {
public:
  void Run() override {
    std::unique_lock lock(m_mutex);
    m_released.wait(lock, [this] { return m_release; });
  }
  void Release() {
    {
      std::lock_guard lock(m_mutex);
      m_release = true;
    }
    m_released.notify_all();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_released;
  bool m_release = false;
};

void TestSynchronousPoolRunsInline() {
  std::atomic<uint32_t> runs {0};
  ShaderWorkerPool pool(0);
  Check(!pool.Async(), "zero threads is synchronous");
  auto job = std::make_shared<CountingJob>(runs, 21);
  pool.Submit(job);
  Check(job->Done(), "inline job is done when Submit returns");
  Check(job->result == 42, "inline job produced its result");
  Check(!job->on_worker, "inline job ran on the caller");
  Check(job->thread == std::this_thread::get_id(), "inline job ran on this thread");
  Check(runs.load() == 1, "inline job ran once");
}

void TestAsyncPoolRunsEachJobOnceOnWorkers() {
  constexpr uint32_t kJobs = 64;
  std::atomic<uint32_t> runs {0};
  std::vector<std::shared_ptr<CountingJob>> jobs;
  {
    ShaderWorkerPool pool(3);
    Check(pool.Async() && pool.ThreadCount() == 3, "three workers");
    for (uint32_t i = 0; i < kJobs; i++) {
      jobs.push_back(std::make_shared<CountingJob>(runs, i));
      pool.Submit(jobs.back());
    }
    for (auto &job : jobs) {
      job->Wait();
      Check(job->Done(), "waited job is done");
    }
  }
  Check(runs.load() == kJobs, "every job ran exactly once");
  for (uint32_t i = 0; i < kJobs; i++) {
    Check(jobs[i]->result == i * 2u, "results are visible after Done()");
    Check(jobs[i]->on_worker, "async jobs run on worker threads");
    Check(jobs[i]->thread != std::this_thread::get_id(), "async jobs do not run on the submitter");
  }
  Check(!ShaderWorkerPool::OnWorkerThread(), "the submitter is not a worker");
}

void TestRunNowTakesAQueuedJob() {
  std::atomic<uint32_t> runs {0};
  ShaderWorkerPool pool(1);
  auto blocker = std::make_shared<BlockingJob>();
  pool.Submit(blocker);
  // Wait until the single worker is inside the blocker, so the next job stays queued.
  for (int i = 0; i < 1000 && pool.Pending() != 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto urgent = std::make_shared<CountingJob>(runs, 5);
  pool.Submit(urgent);
  Check(pool.Pending() == 1, "second job is queued behind the busy worker");
  pool.RunNow(urgent);
  Check(urgent->Done() && urgent->result == 10, "RunNow ran the queued job");
  Check(!urgent->on_worker, "RunNow ran it on the caller");
  Check(pool.Pending() == 0, "RunNow removed it from the queue");
  blocker->Release();
  blocker->Wait();
  // RunNow on a finished job returns without running it again.
  pool.RunNow(urgent);
  Check(runs.load() == 1, "a job never runs twice");
}

void TestRunNowRunsAnUnsubmittedJob() {
  // A blocking caller (compute) runs its job itself without ever queueing it.
  std::atomic<uint32_t> runs {0};
  ShaderWorkerPool pool(2);
  auto job = std::make_shared<CountingJob>(runs, 7);
  pool.RunNow(job);
  Check(job->Done() && job->result == 14 && !job->on_worker, "RunNow ran the unsubmitted job here");
  pool.RunNow(job);
  Check(runs.load() == 1, "RunNow on a finished job does nothing");
}

void TestDestructorDropsQueuedJobs() {
  std::atomic<uint32_t> runs {0};
  auto blocker = std::make_shared<BlockingJob>();
  auto queued = std::make_shared<CountingJob>(runs, 1);
  std::thread releaser;
  {
    ShaderWorkerPool pool(1);
    pool.Submit(blocker);
    for (int i = 0; i < 1000 && pool.Pending() != 0; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pool.Submit(queued);
    releaser = std::thread([blocker] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      blocker->Release();
    });
    // The destructor waits for the running job and drops the queued one.
  }
  releaser.join();
  Check(blocker->Done(), "the running job finished before the pool was destroyed");
  Check(!queued->Done() && runs.load() == 0, "a queued job is dropped unstarted at shutdown");
}

void TestDefaultThreadCount() {
  Check(ShaderWorkerPool::DefaultThreadCount(0) == 1, "no core info: one worker");
  Check(ShaderWorkerPool::DefaultThreadCount(2) == 1, "two cores: one worker");
  Check(ShaderWorkerPool::DefaultThreadCount(4) == 1, "four cores: one worker");
  Check(ShaderWorkerPool::DefaultThreadCount(6) == 2, "six cores: two workers");
  Check(ShaderWorkerPool::DefaultThreadCount(8) == 3, "eight cores (the tablet): three workers");
  Check(ShaderWorkerPool::DefaultThreadCount(32) == 3, "the pool is capped at three workers");
}

} // namespace

int main() {
  TestSynchronousPoolRunsInline();
  TestAsyncPoolRunsEachJobOnceOnWorkers();
  TestRunNowTakesAQueuedJob();
  TestRunNowRunsAnUnsubmittedJob();
  TestDestructorDropsQueuedJobs();
  TestDefaultThreadCount();
  std::printf("ShaderWorkerPoolTests: ok\n");
  return 0;
}

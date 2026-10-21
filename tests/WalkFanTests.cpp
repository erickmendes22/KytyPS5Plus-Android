#include "graphics/host_gpu/renderer/pipeline/walkFan.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <atomic>
#include <fmt/color.h>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <set>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Walk fan-out (graphics/host_gpu/renderer/pipeline/walkFan.h): pool walks equal inline walks,
// the recorder can take a job no worker started, the pickup rule, and a protect landing during
// an in-flight walk (the walk must neither fault nor be accepted).
namespace {

namespace IR  = Libs::Graphics::ShaderRecompiler::IR;
namespace Fan = Libs::Graphics::WalkFan;
namespace WalkReader = Libs::Graphics::WalkReader;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "WalkFanTests: failed: %s\n", text);
		std::abort();
	}
}

std::array<uint32_t, 2> PointerWords(const void* pointer) {
	const auto address = reinterpret_cast<uint64_t>(pointer);
	return {static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
}

// user data (2 words) -> root table {leaf low, leaf high} -> leaf table: three reads, flattened as
// {leaf[1], root[0], root[1], leaf[2]} (the plan of ResourceMaterializationTests'
// TestConcurrentWalksOfOnePlan).
struct ChainPlan {
	IR::Program      program;
	IR::ResourcePlan plan;

	ChainPlan() {
		// SrtProgram(0) of ResourceMaterializationTests: an SRT-backed descriptor source.
		program.stage                      = Libs::Graphics::ShaderType::Compute;
		program.srt_plan_complete          = true;
		program.resource_tracking_complete = true;
		auto block                         = std::make_unique<IR::Block>();
		auto* value_block                  = block.get();
		program.blocks.push_back(value_block);
		value_block->id = 0;
		program.block_storage.push_back(std::move(block));
		IR::MemoryInfo memory;
		memory.kind          = IR::ResourceKind::ScalarAddress;
		memory.planning_only = true;
		program.memory_info.push_back(memory);
		{
			auto& handle = value_block->AppendNewInst(IR::ValueOpcode::GetAddressResource,
			                                          {IR::Value(0u), IR::Value(0u)});
			auto& raw    = value_block->AppendNewInst(
			    IR::ValueOpcode::LoadAddressU32,
			    {IR::Value(&handle), IR::Value(0u), IR::Value(0u), IR::Value(true)});
			raw.SetFlags(IR::MemoryFlags {.index = 0, .pc = 0x40});
			program.srt_reads.push_back({IR::Value(&raw), 0});
			auto& srt  = value_block->AppendNewInst(IR::ValueOpcode::GetSrtResource);
			auto& flat = value_block->AppendNewInst(IR::ValueOpcode::ReadConst, {IR::Value(&srt), IR::Value(0u)});
			IR::DescriptorSource source;
			source.dwords[0]   = IR::Value(&flat);
			source.dwords[1]   = IR::Value(0u);
			source.dword_count = 2;
			program.descriptor_sources.push_back(source);
		}
		program.user_data_base  = 4;
		program.user_data_count = 2;
		const auto emit = [&](IR::ValueOpcode opcode, std::initializer_list<IR::Value> args) {
			return IR::Value(&value_block->AppendNewInst(opcode, args));
		};
		const auto low  = emit(IR::ValueOpcode::GetUserData, {IR::Value(IR::ScalarReg(4))});
		const auto high = emit(IR::ValueOpcode::GetUserData, {IR::Value(IR::ScalarReg(5))});
		const auto root = emit(IR::ValueOpcode::GetAddressResource, {low, high});
		const auto read = [&](IR::Value address, uint32_t offset) {
			const auto value = emit(IR::ValueOpcode::LoadAddressU32,
			                        {address, IR::Value(offset), IR::Value(0u), IR::Value(true)});
			value.Instruction()->SetFlags(IR::MemoryFlags {.index = 0});
			program.srt_reads.push_back({value, static_cast<uint32_t>(program.srt_reads.size())});
			return value;
		};
		const auto pointer_low  = read(root, 0u);
		const auto pointer_high = read(root, 4u);
		auto*      leaf         = program.srt_reads[0].value.Instruction();
		leaf->Arg(0).Instruction()->SetArg(0, pointer_low);
		leaf->Arg(0).Instruction()->SetArg(1, pointer_high);
		leaf->SetArg(1, IR::Value(4u));
		read(leaf->Arg(0), 8u);
		plan = IR::ExtractResourcePlan(program);
	}
};

// The memory side of a test: the GPU-modified set the recorder keeps, its write journal and the
// view publisher (BufferCache::SetWalkJournal / PublishWalkView in the renderer).
struct FakeGpu {
	std::vector<std::pair<uint64_t, uint64_t>> modified;
	WalkReader::WriteJournal                   journal;
	WalkReader::ViewPublisher                  publisher;

	FakeGpu() { Publish(); }
	void Publish() {
		publisher.Publish(journal.Generation(), [&](auto&& visit) {
			auto sorted = modified;
			std::sort(sorted.begin(), sorted.end());
			for (const auto& [begin, end]: sorted) visit(begin, end);
		});
	}
	// The recorder records a GPU write over host bytes [p, p + size).
	void Write(uint64_t begin, uint64_t size) {
		modified.emplace_back(begin, begin + size);
		journal.Note(begin, begin + size);
		Publish();
	}
	void Write(const void* p, uint64_t size) { Write(reinterpret_cast<uint64_t>(p), size); }
};

// Process memory as guest memory: identity fold, plain copies.
const WalkReader::Memory kIdentityMemory {
    .fold         = [](uint64_t address, uint64_t, uint64_t* host) {
	    *host = address;
	    return true;
    },
    .read_backing = [](uint64_t host, void* data, uint64_t size) {
	    std::memcpy(data, reinterpret_cast<const void*>(host), size);
	    return true;
    },
};

std::shared_ptr<Fan::WalkJob> MakeJob(const ChainPlan& chain, std::span<const uint32_t> user_data,
                                      const FakeGpu& gpu) {
	auto job       = std::make_shared<Fan::WalkJob>();
	job->plan      = &chain.plan;
	job->entry     = &chain;
	job->user_data = {user_data.begin(), user_data.end()};
	job->view      = gpu.publisher.Current();
	return job;
}

// Pool walks of many items equal inline walks of the same inputs, in any interleaving.
void TestPoolMatchesInline() {
	FakeGpu                gpu;
	const Fan::WalkContext context {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::SetContext(&context);
	const ChainPlan chain;
	constexpr int   kItems = 3000;
	struct Item {
		std::array<uint32_t, 3>       leaf {};
		std::array<uint32_t, 2>       root {};
		std::array<uint32_t, 2>       user {};
		std::shared_ptr<Fan::WalkJob> job;
	};
	std::vector<Item> items(kItems);
	Fan::WalkPool     pool(2);
	for (int i = 0; i < kItems; i++) {
		auto& item = items[i];
		item.leaf  = {0u, static_cast<uint32_t>(i * 3 + 1), static_cast<uint32_t>(~i)};
		item.root  = PointerWords(item.leaf.data());
		item.user  = PointerWords(item.root.data());
		item.job   = MakeJob(chain, item.user, gpu);
		pool.Submit(item.job);
	}
	int accepted = 0;
	int taken    = 0;
	for (auto& item: items) {
		taken += item.job->TakeOrWait(context) ? 1 : 0;
		IR::ResourcePlan::Scratch  scratch;
		IR::ResourceSnapshot       snapshot;
		IR::ResourceSpecialization specialization;
		const IR::SrtRuntime       runtime {.user_data = item.user};
		Check(IR::MaterializeResources(chain.plan, scratch, runtime, snapshot, specialization),
		      "inline walk failed");
		Check(Fan::SameResult(item.job->resources, item.job->specialization, snapshot, specialization),
		      "a pool walk differs from the inline walk of the same inputs");
		Check(item.job->reads.ranges.size() >= 2u, "the walk's reads were not recorded");
		accepted += Fan::Accept(*item.job, context) == Fan::Outcome::Accepted ? 1 : 0;
	}
	Check(accepted == kItems, "a clean walk with no GPU write since its view was not accepted");
	pool.Quiesce();
	std::printf("WalkFanTests: pool == inline for %d items (%d taken by the recorder)\n", kItems, taken);
}

// The recorder runs a job itself when every worker is busy (it never waits behind the queue).
std::atomic<bool> g_release {false};
std::atomic<bool> g_blocked {false};

void TestRecorderTakesQueuedJob() {
	FakeGpu                       gpu;
	static const WalkReader::Memory kBlockingMemory {
	    .fold         = kIdentityMemory.fold,
	    .read_backing = [](uint64_t host, void* data, uint64_t size) {
		    if (!g_blocked.exchange(true)) {
			    while (!g_release.load()) std::this_thread::yield();
		    }
		    std::memcpy(data, reinterpret_cast<const void*>(host), size);
		    return true;
	    },
	};
	const Fan::WalkContext blocking {.memory = kBlockingMemory, .journal = &gpu.journal};
	const Fan::WalkContext plain {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::SetContext(&blocking);
	const ChainPlan         chain;
	std::array<uint32_t, 3> leaf {0u, 7u, 8u};
	const auto              root = PointerWords(leaf.data());
	const auto              user = PointerWords(root.data());
	Fan::WalkPool           pool(1);
	auto                    first  = MakeJob(chain, user, gpu);
	auto                    second = MakeJob(chain, user, gpu);
	pool.Submit(first);
	while (!g_blocked.load()) std::this_thread::yield();
	pool.Submit(second);
	Check(second->TakeOrWait(plain), "the recorder did not take a job no worker had started");
	Check(Fan::Accept(*second, plain) == Fan::Outcome::Accepted, "the recorder's own walk was rejected");
	g_release = true;
	Check(!first->TakeOrWait(plain), "the recorder re-ran a job a worker had started");
	Check(first->ok, "the worker's walk failed");
	pool.Quiesce();
}

// The pickup rule.
void TestAcceptRules() {
	FakeGpu                gpu;
	const Fan::WalkContext context {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::WalkJob           job;
	job.view_generation = gpu.journal.Generation();
	job.ok              = false;
	Check(Fan::Accept(job, context) == Fan::Outcome::Failed, "a failed walk was accepted");
	job.ok = true;
	job.reads.Refuse(WalkReader::Reads::Refusal::NoBacking);
	Check(job.reads.ranges.empty(), "test setup: no ranges");
	Check(Fan::Accept(job, context) == Fan::Outcome::RefusedNoBacking,
	      "a refused walk with no recorded ranges was accepted");
	job.reads.Clear();
	Check(Fan::Accept(job, context) == Fan::Outcome::Accepted, "a walk with no GPU write since its view");
	uint32_t word = 0;
	const auto at = reinterpret_cast<uint64_t>(&word);
	job.reads.Note(at, at + 4);
	uint32_t other = 0;
	gpu.Write(&other, 4);
	Check(Fan::Accept(job, context) == Fan::Outcome::Accepted, "a GPU write elsewhere invalidated a walk");
	gpu.Write(&word, 4);
	Check(Fan::Accept(job, context) == Fan::Outcome::Invalidated,
	      "a GPU write to a read range did not invalidate the walk");
	// A trimmed journal cannot vouch for an older view, even for unrelated reads.
	job.reads.Clear();
	job.reads.Note(at + 64, at + 68);
	job.view_generation = gpu.journal.Generation() - 1;
	gpu.journal.Trim(gpu.journal.Generation());
	Check(Fan::Accept(job, context) == Fan::Outcome::Invalidated,
	      "a walk whose view is older than the trimmed journal was accepted");
	const Fan::WalkContext no_journal {.memory = kIdentityMemory};
	job.view_generation = gpu.journal.Generation();
	Check(Fan::Accept(job, no_journal) == Fan::Outcome::Invalidated, "a walk was accepted without a journal");
}

// A protect landing while a walk is in flight. Guest memory is one shared file mapped twice: the
// guest view the walk's pointers name, and an alias the worker copies from (GuestBackingStore's
// m_backing_base; the fold maps guest -> alias). Mid-walk the test protects the guest view
// (PROT_NONE) and records a GPU write over both tables. A worker that touched the guest view would
// SIGSEGV; WorkerWalk never does. The walk keeps reading the alias and must come back
// invalidated at pickup (the write was noted after its view), never accepted.
struct AliasMemory {
	uint8_t* guest = nullptr;
	uint8_t* alias = nullptr;
	size_t   size  = 0;
};
AliasMemory       g_alias;
std::atomic<int>  g_reads {0};
std::atomic<bool> g_protect_now {false};
std::atomic<bool> g_protected {false};
int               g_pause_at_read = 0;

const WalkReader::Memory kAliasMemory {
    .fold         = [](uint64_t address, uint64_t size, uint64_t* host) {
	    const auto guest = reinterpret_cast<uint64_t>(g_alias.guest);
	    if (address < guest || address + size > guest + g_alias.size) return false;
	    *host = reinterpret_cast<uint64_t>(g_alias.alias) + (address - guest);
	    return true;
    },
    .read_backing = [](uint64_t host, void* data, uint64_t size) {
	    std::memcpy(data, reinterpret_cast<const void*>(host), size);
	    return true;
    },
};

void TestProtectDuringInFlightWalk() {
	const auto page   = static_cast<size_t>(sysconf(_SC_PAGESIZE));
	char       path[] = "/tmp/kyty_walkfan_XXXXXX";
	const int  fd     = mkstemp(path);
	Check(fd >= 0, "mkstemp");
	unlink(path);
	Check(ftruncate(fd, static_cast<off_t>(page)) == 0, "ftruncate");
	g_alias.size  = page;
	g_alias.guest = static_cast<uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	g_alias.alias = static_cast<uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	Check(g_alias.guest != MAP_FAILED && g_alias.alias != MAP_FAILED, "mmap");
	const ChainPlan chain;
	auto*           root       = reinterpret_cast<uint32_t*>(g_alias.guest);
	auto*           leaf       = reinterpret_cast<uint32_t*>(g_alias.guest + 64);
	const auto      leaf_words = PointerWords(leaf);
	root[0]                    = leaf_words[0];
	root[1]                    = leaf_words[1];
	leaf[1]                    = 0x1111u;
	leaf[2]                    = 0x2222u;
	const auto user            = PointerWords(root);
	static const WalkReader::Memory kPausingMemory {
	    .fold         = [](uint64_t address, uint64_t size, uint64_t* host) {
		    if (g_reads.fetch_add(1) + 1 == g_pause_at_read) {
			    g_protect_now = true;
			    while (!g_protected.load()) std::this_thread::yield();
		    }
		    return kAliasMemory.fold(address, size, host);
	    },
	    .read_backing = kAliasMemory.read_backing,
	};
	int refused     = 0;
	int invalidated = 0;
	// Pause before each read of the walk in turn (the protect lands before that read), and once
	// after all of them (the protect lands after the walk, before pickup).
	for (int pause = 1; pause <= 6; pause++) {
		FakeGpu gpu;
		Check(mprotect(g_alias.guest, page, PROT_READ | PROT_WRITE) == 0, "unprotect");
		g_reads         = 0;
		g_protect_now   = false;
		g_protected     = false;
		g_pause_at_read = pause;
		const Fan::WalkContext context {.memory = kPausingMemory, .journal = &gpu.journal};
		Fan::SetContext(&context);
		Fan::WalkPool pool(1);
		auto          job = MakeJob(chain, user, gpu);
		pool.Submit(job);
		while (!g_protect_now.load() && !job->Done()) std::this_thread::yield();
		// The recorder records a GPU write over both tables: the guest view loses access and the
		// bytes are GPU-modified in the published view from here on.
		Check(mprotect(g_alias.guest, page, PROT_NONE) == 0, "protect");
		gpu.Write(reinterpret_cast<uint64_t>(g_alias.alias), 128);
		// The job's view is the one from submission, so its reads are not refused by this write; the
		// pickup check (writes noted in the journal after that view) must reject it every time.
		g_protected = true;
		job->TakeOrWait(context);
		const auto outcome = Fan::Accept(*job, context);
		Check(outcome != Fan::Outcome::Accepted, "a walk over memory written mid-walk was accepted");
		refused += outcome == Fan::Outcome::RefusedGpuModified ? 1 : 0;
		invalidated += outcome == Fan::Outcome::Invalidated ? 1 : 0;
		pool.Quiesce();
	}
	Check(invalidated == 6, "a walk over memory written after its view was not invalidated");
	munmap(g_alias.guest, page);
	munmap(g_alias.alias, page);
	close(fd);
	std::printf("WalkFanTests: protect mid-walk: %d refused, %d invalidated, 0 faults\n", refused, invalidated);
}

// A walk whose view already has the bytes GPU-modified is refused (and walked inline), and the
// refusal is not hidden by a later readback that makes the bytes clean again.
void TestRefusedByView() {
	FakeGpu                gpu;
	const Fan::WalkContext context {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::SetContext(&context);
	const ChainPlan         chain;
	std::array<uint32_t, 3> leaf {0u, 1u, 2u};
	const auto              root = PointerWords(leaf.data());
	const auto              user = PointerWords(root.data());
	gpu.Write(leaf.data(), sizeof(leaf));
	Fan::WalkPool pool(1);
	auto          job = MakeJob(chain, user, gpu);
	pool.Submit(job);
	job->TakeOrWait(context);
	gpu.modified.clear(); // a readback: the bytes are CPU-visible again
	gpu.Publish();
	Check(Fan::Accept(*job, context) == Fan::Outcome::RefusedGpuModified,
	      "a walk over bytes GPU-modified in its view was not refused");
	pool.Quiesce();
}

// Walk workers run on a large stack (SRT walks recurse deeply; 1 MiB overflowed on device).
std::atomic<size_t> g_worker_stack {0};
size_t CurrentStackBytes() {
#if defined(__APPLE__)
	return pthread_get_stacksize_np(pthread_self());
#else
	pthread_attr_t attr;
	size_t         size = 0;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		pthread_attr_getstacksize(&attr, &size);
		pthread_attr_destroy(&attr);
	}
	return size;
#endif
}

void TestWorkerStackSize() {
	FakeGpu gpu;
	static const WalkReader::Memory kStackProbe {
	    .fold         = kIdentityMemory.fold,
	    .read_backing = [](uint64_t host, void* data, uint64_t size) {
		    g_worker_stack = CurrentStackBytes();
		    std::memcpy(data, reinterpret_cast<const void*>(host), size);
		    return true;
	    },
	};
	const Fan::WalkContext context {.memory = kStackProbe, .journal = &gpu.journal};
	Fan::SetContext(&context);
	const ChainPlan         chain;
	std::array<uint32_t, 3> leaf {0u, 1u, 2u};
	const auto              root = PointerWords(leaf.data());
	const auto              user = PointerWords(root.data());
	Fan::WalkPool           pool(1);
	auto                    job = MakeJob(chain, user, gpu);
	pool.Submit(job);
	pool.Quiesce();
	Check(job->Done() && g_worker_stack.load() >= (8u << 20u), "walk worker stack below 8 MiB");
}

// LookupMemo (fan_memo): a hit needs the full key; a changed static state, stage or hash misses; a
// colliding slot is replaced, never returned for the other key.
void TestLookupMemo() {
	struct Key {
		uint32_t              stage = 0;
		uint64_t              hash  = 0;
		std::vector<uint32_t> state;
		bool operator==(const Key&) const = default;
	};
	Fan::LookupMemo<Key, const int, 4> memo;
	const int a = 1, b = 2;
	const Key ka {1, 0x1234, {1, 2, 3}};
	Check(memo.Find(ka, 5) == nullptr, "empty memo hit");
	memo.Insert(ka, 5, &a);
	Check(memo.Find(ka, 5) == &a, "memo missed its own key");
	Key changed = ka;
	changed.state[2] = 4;
	Check(memo.Find(changed, 5) == nullptr, "a changed static state hit the memo");
	changed = ka;
	changed.stage = 2;
	Check(memo.Find(changed, 5) == nullptr, "a changed stage hit the memo");
	const Key kb {1, 0x9999, {7}};
	memo.Insert(kb, 9, &b); // 9 & 3 == 5 & 3: same slot
	Check(memo.Find(ka, 5) == nullptr, "a replaced slot returned the old key's value");
	Check(memo.Find(kb, 9) == &b, "memo missed the replacing key");
	memo.Clear();
	Check(memo.Find(kb, 9) == nullptr, "Clear kept an entry");
}

// The front's sink submits an item's jobs when its scope ends (one pool lock per item).
void TestSinkSubmitsPendingAtScopeEnd() {
	FakeGpu                gpu;
	const Fan::WalkContext context {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::SetContext(&context);
	const ChainPlan         chain;
	std::array<uint32_t, 3> leaf {0u, 3u, 4u};
	const auto              root = PointerWords(leaf.data());
	const auto              user = PointerWords(root.data());
	Fan::PrewalkList        list;
	std::vector<std::shared_ptr<Fan::WalkJob>> jobs;
	{
		const Fan::ScopedList sink(Fan::t_walk_sink, &list);
		for (int i = 0; i < 3; i++) {
			jobs.push_back(MakeJob(chain, user, gpu));
			Fan::t_walk_sink->PushPending(jobs.back());
		}
	}
	Fan::Pool().Quiesce();
	for (auto& job: jobs) Check(job->Done() && job->ok, "a pending job was not submitted at scope end");
	Fan::Outcome reason = Fan::Outcome::Count;
	Check(list.Next(reason) == jobs[0], "pending jobs keep the request order");
}

// Concurrent walks of one plan WITH control flow and a branch reader (every real walk has one):
// RefreshFlatBuffer's branch walker must use the walk's scratch, not the plan's own (c17_drain1 /
// c17_fan3a: SIGSEGV in ResetUnknowns from racing deque/vector growth in plan.scratch).
std::array<uint32_t, 4> g_branch_table {0u, 0x8000u, 0u, 0u};
bool ReadRealMemory(void*, uint64_t address, std::span<uint32_t> words) {
	std::memcpy(words.data(), reinterpret_cast<const void*>(address), words.size_bytes());
	return true;
}

IR::Block& AddBlock(IR::Program& program) {
	auto block = std::make_unique<IR::Block>();
	auto* result = block.get();
	program.blocks.push_back(result);
	result->id = static_cast<uint32_t>(program.blocks.size() - 1u);
	program.block_storage.push_back(std::move(block));
	return *result;
}

void TestConcurrentBranchWalks() {
	IR::Program program;
	program.stage                      = Libs::Graphics::ShaderType::Compute;
	program.user_data_count            = 1;
	program.srt_plan_complete          = true;
	program.resource_tracking_complete = true;
	auto& block = AddBlock(program);
	program.memory_info.push_back({.kind = IR::ResourceKind::ScalarAddress});
	const auto table = PointerWords(g_branch_table.data());
	auto& handle = block.AppendNewInst(IR::ValueOpcode::GetAddressResource,
	                                   {IR::Value(table[0]), IR::Value(table[1])});
	auto& offset = block.AppendNewInst(IR::ValueOpcode::GetUserData, {IR::Value(static_cast<IR::ScalarReg>(0))});
	auto& read   = block.AppendNewInst(IR::ValueOpcode::LoadAddressU32,
	                                   {IR::Value(&handle), IR::Value(&offset), IR::Value(0u), IR::Value(false)});
	read.SetFlags(IR::MemoryFlags {.index = 0});
	IR::DescriptorSource source;
	source.dwords      = {IR::Value(&read), IR::Value(0u), IR::Value(4u), IR::Value(0u)};
	source.dword_count = 4;
	program.descriptor_sources.push_back(source);
	program.info.buffers.push_back({.source = 0, .written = true});
	auto& condition = block.AppendNewInst(IR::ValueOpcode::IEqual32, {IR::Value(&read), IR::Value(0x8000u)});
	auto& store_block = AddBlock(program);
	AddBlock(program);
	using Kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
	program.blocks[0]->condition                = IR::Value(&condition);
	program.blocks[0]->terminator.kind          = Kind::ConditionalBranch;
	program.blocks[0]->terminator.true_block    = program.blocks[1];
	program.blocks[0]->terminator.false_block   = program.blocks[2];
	program.blocks[1]->terminator.kind          = Kind::Return;
	program.blocks[2]->terminator.kind          = Kind::Return;
	program.memory_info.push_back({.kind = IR::ResourceKind::Buffer, .resource = 0});
	auto& output = store_block.AppendNewInst(IR::ValueOpcode::GetBufferResource,
	    {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
	store_block.AppendNewInst(IR::ValueOpcode::StoreBufferU32,
	    {IR::Value(&output), IR::Value(0u), IR::Value(0u), IR::Value(0u), IR::Value(1u), IR::Value(true)})
	    .SetFlags(IR::MemoryFlags {.index = 1});
	const auto plan = IR::ExtractResourcePlan(program);
	Check(!plan.control_flow.empty(), "test setup: the plan needs control flow");
	const std::array<uint32_t, 1> user_data {4u};
	const IR::SrtRuntime runtime {
	    .user_data                  = user_data,
	    .read_specialization_memory = ReadRealMemory,
	    .read_branch_memory         = ReadRealMemory,
	};
	constexpr int kThreads = 4;
	std::atomic<int> failures {0};
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([&] {
			IR::ResourcePlan::Scratch  scratch;
			IR::ResourceSnapshot       snapshot;
			IR::ResourceSpecialization specialization;
			for (int i = 0; i < 4000; i++) {
				if (!IR::MaterializeResources(plan, scratch, runtime, snapshot, specialization)) failures++;
			}
		});
	}
	for (auto& thread: threads) thread.join();
	Check(failures.load() == 0, "a concurrent branch walk failed");
	// The plan's own scratch must stay untouched by walks on caller-owned scratch.
	Check(plan.scratch.evaluation_contexts.empty(), "a walk on caller scratch used the plan's own scratch");
}

void TestPrewalkListOrder() {
	Fan::PrewalkList list;
	auto             job = std::make_shared<Fan::WalkJob>();
	list.Push(job);
	list.Push(nullptr, Fan::Outcome::NoEntry);
	Fan::Outcome reason = Fan::Outcome::Count;
	Check(list.Next(reason) == job && reason == Fan::Outcome::Count, "first slot");
	Check(list.Next(reason) == nullptr && reason == Fan::Outcome::NoEntry, "inline slot keeps its reason");
	Check(list.Next(reason) == nullptr && reason == Fan::Outcome::Rerequest, "past the end is a re-request");
}

// Resizing the pool while jobs are queued and running loses none of them.
void TestResizeUnderLoad() {
	FakeGpu                gpu;
	const Fan::WalkContext context {.memory = kIdentityMemory, .journal = &gpu.journal};
	Fan::SetContext(&context);
	const ChainPlan         chain;
	std::array<uint32_t, 3> leaf {0u, 5u, 6u};
	const auto              root = PointerWords(leaf.data());
	const auto              user = PointerWords(root.data());
	Fan::WalkPool           pool(1);
	std::vector<std::shared_ptr<Fan::WalkJob>> jobs;
	for (int i = 0; i < 2000; i++) {
		jobs.push_back(MakeJob(chain, user, gpu));
		pool.Submit(jobs.back());
		if (i == 500) pool.Resize(4);
		if (i == 1200) pool.Resize(2);
		if (i == 1700) pool.Resize(1);
	}
	pool.Quiesce();
	for (const auto& job: jobs) Check(job->Done() && job->ok, "a job was lost or failed across a resize");
	Check(pool.Workers() == 1, "resize");
}

} // namespace

namespace Common {
int DbgExitHandler(const char*, int, std::string_view) {
	std::abort();
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) {
	std::abort();
}
int DbgExitIfHandler(const char*, const char*, int) {
	return 1;
}
void DbgExit(int) {
	std::abort();
}
} // namespace Common

int main() {
	TestPrewalkListOrder();
	TestConcurrentBranchWalks();
	TestLookupMemo();
	TestSinkSubmitsPendingAtScopeEnd();
	TestWorkerStackSize();
	TestAcceptRules();
	TestPoolMatchesInline();
	TestRecorderTakesQueuedJob();
	TestResizeUnderLoad();
	TestProtectDuringInFlightWalk();
	TestRefusedByView();
	std::printf("WalkFanTests: ok\n");
	return 0;
}

// The typed-IR implementation set, amalgamated as in ResourceMaterializationTests.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"

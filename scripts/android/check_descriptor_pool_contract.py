#!/usr/bin/env python3
"""Static cross-check of shader descriptor demand against Vulkan pool capacity.

Runs before the native Android build to prevent a descriptorHeap.cpp:62
regression (reported for PPSA03671). This verifies declarations, not GPU
driver behavior, which still needs a device test.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
heap = (ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp").read_text()
descriptors = (ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptors.cpp").read_text()
header = (ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.h").read_text()

pool_block = heap.split("constexpr std::array DescriptorPoolSizes = {", 1)[1].split("};", 1)[0]
pool_entries = re.findall(
    r"vk::DescriptorPoolSize\s*\{\s*vk::DescriptorType::(e\w+)\s*,\s*(\d+)\s*\}", pool_block
)
assert pool_entries, "Descriptor pool declarations disappeared"
pools = {name: int(count) for name, count in pool_entries}
assert len(pools) == len(pool_entries), "Duplicate descriptor types in a single pool"
required = set(re.findall(r"vk::DescriptorType::(e\w+)", descriptors.split(
    "vk::DescriptorType NativeDescriptorType(", 1)[1].split("\n}", 1)[0]))
required.discard("eCount")
missing = sorted(required - pools.keys())
assert not missing, "Shader requests types not supported by the heap: " + ", ".join(missing)
assert pools["eUniformBuffer"] >= 32, "Wolverine uniform descriptor allocation would exhaust new pool"
assert all(v >= 32 for v in pools.values()), "Too few descriptors for a full batch"
assert "m_sets.clear()" in heap, "Old descriptor sets must be invalidated at pool rollover"
assert "m_master_semaphore.IsFree(tick)" in heap, "Pool recycling must wait for GPU completion"
print("Descriptor contract OK:", pools, "required =", sorted(required))

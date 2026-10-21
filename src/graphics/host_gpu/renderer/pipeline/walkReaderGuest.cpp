// WalkReader::GuestMemory (walkReader.h): the guest memory a prepare worker reads, kept out of
// walkReader.cpp so the reader logic links into host tests without the kernel.
#include "graphics/host_gpu/renderer/pipeline/walkReader.h"
#include "kernel/addressFold.h"
#include "kernel/memory.h"

namespace Libs::Graphics::WalkReader {

Memory GuestMemory() {
	return {
	    .fold = [](uint64_t address, uint64_t size, uint64_t* host) {
		    return ::Kernel::AddressFold::FoldSpan(address, size, host);
	    },
	    // The direct-memory alias: never faults, whatever the guest view's protection.
	    .read_backing = [](uint64_t host_address, void* data, uint64_t size) {
		    return LibKernel::Memory::TryReadBacking(host_address, data, size);
	    },
	};
}

} // namespace Libs::Graphics::WalkReader

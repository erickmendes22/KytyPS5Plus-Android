#ifndef KYTY_COMMON_HOST_FAULT_ACCESS_H_
#define KYTY_COMMON_HOST_FAULT_ACCESS_H_

// Dependency-free so translator glue (whose include paths shadow Kyty's common headers) can use it.
namespace Common::HostException {

// True when the synchronous memory fault described by a Linux signal frame (ucontext_t) was a write.
bool IsWriteFault(const void* native_context);

// Diagnostics: the faulting pc, link register and frame pointer of this thread's last
// translator-delegated SIGSEGV (ARM64 hosts; zero elsewhere). Recorded without dereferencing.
void RecordFaultRegisters(const void* native_context);
void LastFaultRegisters(unsigned long long* pc, unsigned long long* lr, unsigned long long* fp);

} // namespace Common::HostException

#endif /* KYTY_COMMON_HOST_FAULT_ACCESS_H_ */

#ifndef KYTY_COMMON_HOST_FAULT_ACCESS_H_
#define KYTY_COMMON_HOST_FAULT_ACCESS_H_

// Dependency-free so translator glue (whose include paths shadow Kyty's common headers) can use it.
namespace Common::HostException {

// True when the synchronous memory fault described by a Linux signal frame (ucontext_t) was a write.
bool IsWriteFault(const void* native_context);

} // namespace Common::HostException

#endif /* KYTY_COMMON_HOST_FAULT_ACCESS_H_ */

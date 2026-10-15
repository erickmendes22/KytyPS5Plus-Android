#ifndef KYTY_COMMON_ATOMIC_REF_H_
#define KYTY_COMMON_ATOMIC_REF_H_

#include <atomic>

namespace Common {

#if defined(__cpp_lib_atomic_ref)

template <typename T>
using AtomicRef = std::atomic_ref<T>;

#else

// std::atomic_ref is missing from the libc++ shipped with the Android NDK. This is the
// subset used by the emulator (load, store, compare_exchange_strong) on top of the
// GCC/Clang __atomic builtins, which give the same guarantees on plain lvalues.
template <typename T>
class AtomicRef {
public:
	explicit AtomicRef(T& ref) : m_ref(ref) {}

	T load(std::memory_order order) const { return __atomic_load_n(&m_ref, ToBuiltin(order)); }

	void store(T value, std::memory_order order) const {
		__atomic_store_n(&m_ref, value, ToBuiltin(order));
	}

	bool compare_exchange_strong(T& expected, T desired, std::memory_order success,
	                             std::memory_order failure) const {
		return __atomic_compare_exchange_n(&m_ref, &expected, desired, false, ToBuiltin(success),
		                                   ToBuiltin(failure));
	}

private:
	static int ToBuiltin(std::memory_order order) {
		switch (order) {
			case std::memory_order_relaxed: return __ATOMIC_RELAXED;
			case std::memory_order_consume: return __ATOMIC_CONSUME;
			case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
			case std::memory_order_release: return __ATOMIC_RELEASE;
			case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
			case std::memory_order_seq_cst: return __ATOMIC_SEQ_CST;
		}
		return __ATOMIC_SEQ_CST;
	}

	T& m_ref;
};

#endif

} // namespace Common

#endif // KYTY_COMMON_ATOMIC_REF_H_

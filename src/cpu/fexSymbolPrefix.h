#ifndef KYTY_CPU_FEX_SYMBOL_PREFIX_H_
#define KYTY_CPU_FEX_SYMBOL_PREFIX_H_

// Force-included (CMake -include) into every FEX translation unit and before any FEX header in the
// guest CPU backend. FEX bundles its own fmt and xxHash; renaming their ABI namespace and symbol
// prefix keeps them from colliding with Kyty's copies in the same link. Must stay in sync with the
// FEX build flags in CMakeLists.txt.

#define FMT_BEGIN_NAMESPACE \
	namespace fmt {         \
	inline namespace fexv12 {
#define FMT_END_NAMESPACE \
	}                     \
	}

#define XXH_NAMESPACE KYTY_FEX_

#endif /* KYTY_CPU_FEX_SYMBOL_PREFIX_H_ */

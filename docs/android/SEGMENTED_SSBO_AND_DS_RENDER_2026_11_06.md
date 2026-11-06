# Android storage buffers and persistent missing rendering

Evidence: Wolverine diagnostics 20261106-110051 abort explicitly in NativeStorageBuffer with `storage buffer range is unsupported`, after reaching frame 1609. This is a Vulkan descriptor range guard; the captured log does not include the rejected size. Demon diagnostics 20261106-110419 continue rendering, but the user reports scenery remains missing even after waiting 30–60 seconds. Android logs explicitly report skipped draws while async shader pipelines compile.

The previous Bink pthread identity manifest fix is retained.

## Implementation

Finite storage buffers exceeding a physical descriptor range use power-of-two slices. The specialization reserves enough descriptors for the range and alignment prefix; runtime binding preserves the original backing allocation and logical length. Shader accesses choose the descriptor slice and local element index. Loads, stores and atomics use the same mapping. Logical bounds are checked before selecting a slice, including when robust buffer loads are otherwise enabled. The existing 32-bit byte-address domain bounds slice reservation; huge raw record-count products still reach the existing mapped-range check without truncating their descriptor words. A resolved view whose logical length cannot be represented is rejected explicitly; unbounded sentinel ranges keep their existing conservative cap. The cache signature includes the device slice size.

PPSA01341 awaits required graphics pipelines instead of dropping their draws. It also enables buffer re-resolution when overlapping cache joins retire previously resolved bindings. The rebind mechanism is adapted from Jetsku/KytyPS5 d93c804e (chenxiao07 ed6f063e1) and checks graphics, direct compute, indirect compute arguments and retrace dispatches. Other titles retain their async shader setting and do not enable this rebind option automatically.

## Validation scope

CI validates emitted Vulkan SPIR-V for segmented 8/16/32-bit loads and stores, scalar reads, and 64-bit atomics, with robust-load optimization on and off. Resource materialization tests exercise finite ranges above 128 MiB without altering the guest descriptor, and verify the physical limit survives cached resource plans. Existing shader and Android launch-isolation regressions also run. These checks do not establish rendering correctness or gameplay stability on Android; the resulting APK still requires the user's device test.

#ifndef MOCKTAIL_GRAPHICS_RUST_KERNEL_TOGGLE_H_
#define MOCKTAIL_GRAPHICS_RUST_KERNEL_TOGGLE_H_

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace mocktail::graphics::internal {

// The A/B switch behind one of the MOCKTAIL_RUST_* environment variables, which
// choose between the Rust kernels and the C++ they replaced.
//
// Two properties, and the second exists because the first was not enough:
//
//   * The environment is read once and then cached. These are consulted per
//     block and per texel row, and getenv is a linear scan of the environment.
//
//   * The cache is a relaxed atomic behind an override slot, rather than a
//     function-local `static`. A latching `static` reads the environment on the
//     first call and ignores any setenv after that, which is not a subtlety
//     anybody should have to rediscover: kernel_benchmark set the variable to
//     "on" before its Rust column and to "off" before its C++ column, and
//     because the first call to happen was from the C++ column, both columns
//     ran the C++ path. Every ETC2 and pooled ratio it printed was therefore
//     1.00x against itself. The resampler did not show it, because that
//     benchmark's Rust column calls the FFI directly instead of going through
//     the dispatcher.
//
// Installing an override is explicit and wins over the environment for the rest
// of the process. Clearing it returns to the environment's value, which stays
// latched so the toggle costs the same after the first call either way.
class RustKernelToggle {
 public:
  // Not `kUnset` by accident: an override of -1 is how a caller says "use the
  // environment", so it must be distinguishable from "off" and "on".
  static constexpr int kUnset = -1;

  explicit RustKernelToggle(const char* variable) : variable_(variable) {}

  bool Enabled() const {
    const int override_value = override_.load(std::memory_order_relaxed);
    if (override_value != kUnset) {
      return override_value != 0;
    }
    return EnvironmentDefault();
  }

  // `value` of `kUnset` clears the override.
  void Override(int value) {
    override_.store(value, std::memory_order_relaxed);
  }

 private:
  bool EnvironmentDefault() const {
    // Relaxed is sufficient: every writer stores the same value, so two threads
    // racing on the first call cost one duplicated getenv and nothing else.
    int cached = default_.load(std::memory_order_relaxed);
    if (cached == kUnset) {
      const char* value = std::getenv(variable_);
      // Unset means on: the Rust kernels are the default build.
      cached = (value == nullptr || std::strcmp(value, "off") != 0) ? 1 : 0;
      default_.store(cached, std::memory_order_relaxed);
    }
    return cached != 0;
  }

  const char* variable_;
  mutable std::atomic<int> default_{kUnset};
  std::atomic<int> override_{kUnset};
};

// The three toggles, one per environment variable. These are inline functions
// holding function-local statics, which C++17 guarantees is one instance across
// every translation unit, so a test target that links only the bridge can still
// reach them without dragging the decoder in with it.
inline RustKernelToggle& EtcToggle() {
  static RustKernelToggle toggle("MOCKTAIL_RUST_ETC");
  return toggle;
}

inline RustKernelToggle& PoolToggle() {
  static RustKernelToggle toggle("MOCKTAIL_RUST_POOL");
  return toggle;
}

inline RustKernelToggle& ResampleToggle() {
  static RustKernelToggle toggle("MOCKTAIL_RUST_KERNELS");
  return toggle;
}

}  // namespace mocktail::graphics::internal

#endif  // MOCKTAIL_GRAPHICS_RUST_KERNEL_TOGGLE_H_
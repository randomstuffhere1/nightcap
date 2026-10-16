#ifndef MOCKTAIL_GRAPHICS_MOCKTAIL_RS_BRIDGE_H_
#define MOCKTAIL_GRAPHICS_MOCKTAIL_RS_BRIDGE_H_

// The C++-side half of the Rust kernel library. The C ABI itself is in
// mocktail_rs.h; this file carries the two things the runtime actually asks,
// and the translation unit behind it also holds the static_asserts that keep
// the EtcFormat discriminants in step between the two languages.

namespace mocktail::graphics {

// The SIMD class the kernels will use on this host, as the runtime logs it:
// 0 portable, 1 SSE2, 2 SSSE3, 3 AVX2, 4 NEON. Diagnostics only -- switching
// this would change what the kernels pick, which is not what it is for.
int RustSimdLevel();

// The A/B switches that choose between the Rust kernels and the C++ they
// replaced, overridable from code for the life of the process.
//
// The environment variables are the intended control surface -- MOCKTAIL_RUST_ETC,
// MOCKTAIL_RUST_POOL and MOCKTAIL_RUST_KERNELS=off. They are read once, because
// they are consulted per block. That makes setenv useless for switching them
// after the first texture upload, which is the situation a benchmark that times
// both variants in one process is in, and is why these exist. Pass a non-zero
// value to force one implementation on, zero to force the other, or -1 to go
// back to the environment.
//
// These are for tests. The runtime has no reason to call them.
void SetRustEtcToggle(int value);
void SetRustPoolToggle(int value);
void SetRustResampleToggle(int value);

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_MOCKTAIL_RS_BRIDGE_H_

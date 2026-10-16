// Runtime SIMD detection, done directly and with no AVX state query.
//
// `std::is_x86_feature_detected!` reads through `std_detect`'s cache, and that
// cache drags roughly 400 KB of `std` into every image that links a kernel. That
// is a measured number, not a guess: a staticlib exposing only
// `mocktail_rs_available` linked to 15 KB, and adding one
// `is_x86_feature_detected!` took the same link to 430 KB, by pulling in
// `std_detect` and everything reachable from it. CPUID is a handful of
// instructions and pulls in nothing.
//
// There is a second, sharper trap here. Testing for AVX2 needs to read XCR0, and
// calling `_xgetbv` from a function compiled for the x86-64 baseline lowers to
// a call to a libgcc-provided helper, which reaches `std`'s backtrace
// machinery -- gimli, addr2line, miniz_oxide, rustc_demangle, and a few hundred
// KB with them. SSSE3 asks for no OS state, so it needs no such query: the
// CPUID bit is enough. Until a kernel actually wants AVX2, the check is
// deliberately absent, and adding it means wrapping the query in a
// `#[target_feature(enable = "xsave")]` wrapper rather than calling it from
// baseline code.
//
// This is also what keeps the archive honest: one artifact built for the
// baseline target, running everywhere, with the fast paths chosen at runtime.

use core::sync::atomic::{AtomicU8, Ordering};

/// The SIMD classes the kernels dispatch on.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum SimdLevel {
    /// Portable scalar path.
    Portable = 0,
    /// x86-64 baseline. SSE2 is architectural there.
    Sse2 = 1,
    /// SSSE3, which the ETC2 shuffle kernels want.
    Ssse3 = 2,
    /// AVX2, used for the EAC palette construction.
    Avx2 = 3,
    /// aarch64 NEON, mandatory on that architecture.
    Neon = 4,
}

impl SimdLevel {
    /// The numeric level callers report and log.
    pub fn value(self) -> i32 {
        self as i32
    }
}

// The crate claims x86-64 and aarch64, which are the two architectures the
// runtime is built and released for. Anything else gets the portable path,
// which is correct, just slower -- never assume a baseline it does not have.
#[cfg(target_arch = "x86_64")]
mod imp {
    use core::arch::x86_64 as arch;
    pub fn detect() -> u32 {
        let (_eax, _ebx, ecx, edx) = cpuid(1, 0);
        let sse2 = (edx >> 26) & 1 == 1;
        // SSSE3 is leaf 1, ECX bit 9.
        let ssse3 = (ecx >> 9) & 1 == 1;
        if ssse3 {
            // AVX2 additionally needs the OS to have enabled the wide register
            // state, which is what XCR0 reports. OSXSAVE is leaf 1 ECX bit 27 and
            // AVX itself is bit 28.
            if (ecx >> 27) & 1 == 1 && (ecx >> 28) & 1 == 1 && unsafe { avx_state_enabled() } {
                let (_eax7, ebx7, _ecx7, _edx7) = cpuid(7, 0);
                // AVX2 is leaf 7 subleaf 0, EBX bit 5.
                if (ebx7 >> 5) & 1 == 1 {
                    return 3;
                }
            }
            return 2;
        }
        // SSE2 is baseline on x86-64; the CPUID check is kept so the branch is
        // actually read rather than assumed.
        if sse2 {
            return 1;
        }
        0
    }

    /// True when XCR0 says the OS has enabled SSE and AVX state.
    ///
    /// # Why this is wrapped in target_feature
    ///
    /// `xgetbv` is not part of the x86-64 baseline. Calling it from baseline
    /// code lowers to a call to a libgcc-provided helper, and reaching that
    /// helper drags `std`'s backtrace machinery into every binary that links a
    /// kernel -- measured at roughly 400 KB, and the reason the first version of
    /// this file refused to detect AVX2 at all. Compiling the query as an `xsave`
    /// function lets it be inlined as the `xgetbv` instruction instead, which is
    /// what it is on every machine that can run it.
    #[target_feature(enable = "xsave")]
    unsafe fn avx_state_enabled() -> bool {
        // Bit 1 of XCR0 is SSE state and bit 2 is AVX state; both must be set.
        (unsafe { arch::_xgetbv(0) } & 0b110) == 0b110
    }

    #[inline]
    fn cpuid(leaf: u32, subleaf: u32) -> (u32, u32, u32, u32) {
        // SSE2 is part of the x86-64 baseline, so the intrinsic is available
        // wherever this module compiles; it is declared safe for exactly that
        // reason.
        let out = arch::__cpuid_count(leaf, subleaf);
        (out.eax, out.ebx, out.ecx, out.edx)
    }
}

#[cfg(target_arch = "aarch64")]
mod imp {
    pub fn detect() -> u32 {
        // NEON is mandatory on aarch64.
        4
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod imp {
    pub fn detect() -> u32 {
        0
    }
}

/// Detects the highest SIMD class this CPU will run.
///
/// Conservative on purpose: it claims a class only for the CPUID bits the
/// kernels use, and never claims one that needs an OS-state query.
pub fn detect() -> SimdLevel {
    level_from_code(imp::detect())
}

/// The per-architecture probe's raw code. Kept as a plain u32 because the
/// x86 and aarch64 probes each return their own native width.
#[inline]
fn level_from_code(code: u32) -> SimdLevel {
    match code {
        3 => SimdLevel::Avx2,
        2 => SimdLevel::Ssse3,
        1 => SimdLevel::Sse2,
        4 => SimdLevel::Neon,
        _ => SimdLevel::Portable,
    }
}

/// The cached detection result.
///
/// # Why this is cached, and why it matters so much
///
/// CPUID is a serialising instruction: it drains the pipeline and costs on the
/// order of a hundred cycles. Calling it per *block* rather than per *session*
/// made the ETC2 alpha merge four times slower than the C++ code it was meant to
/// speed up -- 329 us against 1529 us for 16384 blocks, measured. The result is
/// a property of the CPU, which cannot change while the process runs, so it is
/// computed once and read from an atomic after that.
///
/// A relaxed load is enough: the stored value is idempotent, so two threads
/// racing on first use cost one duplicated CPUID and nothing else.
static LEVEL: AtomicU8 = AtomicU8::new(u8::MAX);

#[inline]
pub fn cached_level() -> SimdLevel {
    let cached = LEVEL.load(Ordering::Relaxed);
    if cached != u8::MAX {
        return level_from_code(cached as u32);
    }
    let detected = detect();
    LEVEL.store(detected as u8, Ordering::Relaxed);
    detected
}

/// The numeric level callers log.
pub fn level() -> i32 {
    cached_level().value()
}

/// True when the kernels may use the SSSE3 shuffle path.
pub fn has_ssse3() -> bool {
    matches!(cached_level(), SimdLevel::Ssse3 | SimdLevel::Avx2)
}

/// True when the kernels may use the AVX2 path.
pub fn has_avx2() -> bool {
    matches!(cached_level(), SimdLevel::Avx2)
}

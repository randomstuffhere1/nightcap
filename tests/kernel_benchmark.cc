// Timing harness for the ported kernels.
//
// Not a correctness test -- the differential suite is kernel_parity_test, and
// nothing here asserts. This exists so "the Rust kernel is faster" is a number
// rather than a claim, and so the same numbers can be taken before and after.
//
//   kernel_benchmark [--iters N] [--filter NAME]
//
// Reports nanoseconds per call for each case, the ratio against the C++ oracle
// where there is one, and checksums the output so a compiler cannot elide the
// work. Run it pinned to one core for a stable reading:
//   taskset -c 2 build/tests/kernel_benchmark

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/mocktail_rs.h"
#include "mocktail/graphics/mocktail_rs_bridge.h"
#include "mocktail/graphics/texture_override.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace graphics = mocktail::graphics;

using graphics::EtcBlockBytes;
using graphics::EtcDecodedTexelBytes;
using graphics::EtcFormat;

// A fixed corpus, so two runs of this binary measure the same work.
class Corpus {
 public:
  explicit Corpus(std::uint64_t seed) : state_(seed) {}

  std::uint8_t Byte() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint8_t>(state_ >> 56);
  }

  void Fill(std::uint8_t* out, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      out[i] = Byte();
    }
  }

 private:
  std::uint64_t state_;
};

std::uint64_t Checksum(const std::uint8_t* data, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// One timed trial: `iters` back-to-back calls, no other work inside the loop.
double TimeTrial(int iters, void (*body)(void*), void* context) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    body(context);
  }
  const auto end = std::chrono::steady_clock::now();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                 .count()) /
         static_cast<double>(iters);
}

// Median of several trials, which is the number to believe.
//
// A single trial on a shared machine is worth very little: an earlier version of
// this harness reported the ETC2 alpha merge as a 5% *slowdown* on a case that
// never calls it, because one trial on a noisy core is dominated by whatever
// else the machine was doing. The median over interleaved trials, with the two
// variants alternating so neither gets a systematically warmer cache, is what
// makes the difference between a measurement and an artefact.
// Both statistics, because neither alone is trustworthy on a shared machine.
//
// The minimum is the least-interfered run, so it is the closest thing to the
// kernel's true cost and is what the ratio is computed from. The median is
// reported alongside it as a check: when the two disagree, the machine was busy
// and the run should be repeated rather than believed. A single timed run of
// this harness varied by 40% between repetitions, which is why.
struct Timing {
  double best_ns;
  double median_ns;
};

Timing TimeTrials(int iters, int trials, void (*body)(void*), void* context) {
  std::vector<double> samples;
  samples.reserve(trials);
  // One untimed pass so neither variant pays for a cold branch predictor.
  body(context);
  for (int t = 0; t < trials; ++t) {
    samples.push_back(TimeTrial(iters, body, context));
  }
  std::sort(samples.begin(), samples.end());
  return Timing{samples.front(), samples[samples.size() / 2]};
}

int g_iters = 0;
int g_trials = 9;
std::string g_filter;

// ---------------------------------------------------------------- resample

struct ResampleWork {
  const std::uint8_t* source;
  std::size_t stride;
  std::uint32_t sw, sh, w, h;
  std::uint8_t* cpp_dest;
  std::uint8_t* rs_dest;
};

void ResampleCpp(void* context) {
  ResampleWork* work = static_cast<ResampleWork*>(context);
  // ResampleRgba dispatches into the Rust library, so the C++ column has to
  // turn it off explicitly. Without this both columns measure the same code
  // and every ratio reads 1.0x.
  //
  // This goes through SetRustResampleToggle rather than setenv because the
  // dispatcher latches the environment on its first call and ignores any
  // setenv after that -- which is how the ETC2 and pooled columns below came to
  // time the C++ path twice over and report a flawless 1.00x.
  graphics::SetRustResampleToggle(0);
  graphics::ResampleRgba(work->source, work->sw, work->sh, work->stride,
                         work->w, work->h, work->cpp_dest);
}

void ResampleRust(void* context) {
  ResampleWork* work = static_cast<ResampleWork*>(context);
  mocktail_rs_resample_rgba(work->source, work->sw, work->sh, work->stride,
                            work->w, work->h, work->rs_dest);
}

void RunResample(const char* name, std::uint32_t sw, std::uint32_t sh,
                 std::uint32_t scale) {
  if (!g_filter.empty() && name != g_filter) return;
  const std::uint32_t dw = sw * scale;
  const std::uint32_t dh = sh * scale;
  const std::size_t stride = static_cast<std::size_t>(sw) * 4;
  std::vector<std::uint8_t> source(stride * sh);
  Corpus(0x9e3779b97f4a7c15ULL).Fill(source.data(), source.size());
  std::vector<std::uint8_t> cpp_dest(static_cast<std::size_t>(dw) * dh * 4);
  std::vector<std::uint8_t> rs_dest(cpp_dest.size());

  ResampleWork work{};
  work.source = source.data();
  work.stride = stride;
  work.sw = sw;
  work.sh = sh;
  work.w = dw;
  work.h = dh;
  work.cpp_dest = cpp_dest.data();
  work.rs_dest = rs_dest.data();
  // Both variants write their own destination, so neither is handed a cache
  // the other warmed, and the checksums are compared after the timing rather
  // than inside it -- summing the output is a real cost and would show up as if
  // it were part of the kernel.
  const Timing cpp = TimeTrials(g_iters, g_trials, ResampleCpp, &work);
  const Timing rust = TimeTrials(g_iters, g_trials, ResampleRust, &work);
  const std::uint64_t cpp_sum = Checksum(cpp_dest.data(), cpp_dest.size());
  const std::uint64_t rs_sum = Checksum(rs_dest.data(), rs_dest.size());
  std::printf("  %-22s %8.1f cpp | %8.1f rust | %5.2fx (med %5.2fx) | %s\n", name,
              cpp.best_ns / 1000.0, rust.best_ns / 1000.0,
              cpp.best_ns / rust.best_ns, cpp.median_ns / rust.median_ns,
              cpp_sum == rs_sum ? "identical" : "DIFFER!");
}

// ------------------------------------------------------------- etc2 decode

struct EtcWork {
  EtcFormat format;
  const std::uint8_t* source;
  std::size_t source_bytes;
  std::uint32_t width, height;
  std::uint8_t* destination;
  std::size_t destination_bytes;
};

void EtcCpp(void* context) {
  EtcWork* work = static_cast<EtcWork*>(context);
  // As with the resampler, the C++ path has to be selected explicitly, and it
  // has to be selected through the override rather than setenv: the dispatcher
  // reads MOCKTAIL_RUST_ETC once, on its first call, and caches it for the life
  // of the process.
  graphics::SetRustEtcToggle(0);
  graphics::DecodeEtcImageBlockRows(work->format, work->source,
                                    work->source_bytes, work->width,
                                    work->height, 0,
                                    (work->height + 3) / 4, work->destination,
                                    work->destination_bytes);
}

void EtcRust(void* context) {
  EtcWork* work = static_cast<EtcWork*>(context);
  graphics::SetRustEtcToggle(1);
  graphics::DecodeEtcImageBlockRows(work->format, work->source,
                                    work->source_bytes, work->width,
                                    work->height, 0,
                                    (work->height + 3) / 4, work->destination,
                                    work->destination_bytes);
}

const char* FormatName(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgb8: return "ETC2_RGB8";
    case EtcFormat::kEtc2Rgb8A1: return "ETC2_RGB8A1";
    case EtcFormat::kEtc2Rgba8: return "ETC2_RGBA8";
    case EtcFormat::kEacR11: return "EAC_R11";
    case EtcFormat::kEacR11Signed: return "EAC_R11_SIGNED";
    case EtcFormat::kEacRg11: return "EAC_RG11";
    case EtcFormat::kEacRg11Signed: return "EAC_RG11_SIGNED";
  }
  return "?";
}

void RunEtc(const char* label, EtcFormat format, std::uint32_t width,
            std::uint32_t height) {
  const char* name = FormatName(format);
  std::string full = std::string("etc2_") + name + "_" + label;
  if (!g_filter.empty() && full != g_filter) return;

  const std::uint32_t blocks_wide = (width + 3) / 4;
  const std::uint32_t blocks_high = (height + 3) / 4;
  const std::size_t source_bytes =
      static_cast<std::size_t>(blocks_wide) * blocks_high * EtcBlockBytes(format);
  const std::size_t destination_bytes =
      static_cast<std::size_t>(width) * height * EtcDecodedTexelBytes(format);

  // A corpus that reaches every ETC2 mode rather than random noise: random bytes
  // would hit the fallback modes almost every time and understate the work.
  std::vector<std::uint8_t> source(source_bytes);
  Corpus corpus(0x2545f4914f6cdd1dULL ^ (static_cast<std::uint64_t>(format) << 32));
  for (std::size_t i = 0; i < source_bytes; ++i) {
    source[i] = corpus.Byte();
  }
  // Force a spread of mode selectors across the corpus.
  for (std::size_t block = 0; block * EtcBlockBytes(format) < source_bytes;
       ++block) {
    std::uint8_t* b = source.data() + block * EtcBlockBytes(format);
    b[0] = static_cast<std::uint8_t>(block * 31);
    b[1] = static_cast<std::uint8_t>(block * 17);
    b[3] = static_cast<std::uint8_t>(block * 7);
    b[4] = static_cast<std::uint8_t>(block * 13);
    b[5] = static_cast<std::uint8_t>(block * 5);
    b[6] = static_cast<std::uint8_t>(block * 11);
    b[7] = static_cast<std::uint8_t>(block * 3);
  }

  // One destination per variant: sharing one would hand whichever ran second a
  // cache the first had just written, which showed up as a 20% swing on cases
  // that do not call Rust at all.
  std::vector<std::uint8_t> cpp_destination(destination_bytes);
  std::vector<std::uint8_t> rust_destination(destination_bytes);
  EtcWork work{};
  work.format = format;
  work.source = source.data();
  work.source_bytes = source_bytes;
  work.width = width;
  work.height = height;
  work.destination = cpp_destination.data();
  work.destination_bytes = destination_bytes;
  const Timing cpp = TimeTrials(g_iters, g_trials, EtcCpp, &work);
  work.destination = rust_destination.data();
  const Timing rust = TimeTrials(g_iters, g_trials, EtcRust, &work);
  work.destination = cpp_destination.data();
  const std::uint64_t cpp_sum = Checksum(cpp_destination.data(), destination_bytes);
  const std::uint64_t rust_sum = Checksum(rust_destination.data(), destination_bytes);
  std::printf("  %-22s %8.1f cpp | %8.1f rust | %5.2fx (med %5.2fx) | %s\n",
              full.c_str(), cpp.best_ns / 1000.0, rust.best_ns / 1000.0,
              cpp.best_ns / rust.best_ns, cpp.median_ns / rust.median_ns,
              cpp_sum == rust_sum ? "identical" : "DIFFER!");
}


// ------------------------------------------------------------- batched jobs

// DecodeEtcJobs is the path the Vulkan stub actually uses: one call per upload
// batch, fanned across the worker pool. The single-image cases above never touch
// the pool at all, so they say nothing about it.
//
// This measures the fan-out, which is what the rayon pool replaces: the C++ pool
// declines when it is already busy and drains the whole batch on its caller,
// while rayon steals. So the interesting number is a batch large enough that the
// pool matters, run with the pool enabled and disabled.
struct BatchWork {
  std::vector<graphics::EtcDecodeJob> jobs;
  unsigned workers;
  std::uint64_t sink;
};

void BatchCpp(void* context) {
  BatchWork* work = static_cast<BatchWork*>(context);
  graphics::SetRustPoolToggle(0);
  graphics::DecodeEtcJobs(work->jobs.data(), work->jobs.size(), work->workers);
  for (const auto& job : work->jobs) {
    work->sink += Checksum(job.destination, job.destination_bytes);
  }
}

void BatchRust(void* context) {
  BatchWork* work = static_cast<BatchWork*>(context);
  graphics::SetRustPoolToggle(1);
  graphics::DecodeEtcJobs(work->jobs.data(), work->jobs.size(), work->workers);
  for (const auto& job : work->jobs) {
    work->sink += Checksum(job.destination, job.destination_bytes);
  }
}

void RunBatch(const char* label, EtcFormat format, std::uint32_t width,
              std::uint32_t height, std::uint32_t images, unsigned workers) {
  const std::string name = std::string("batch_") + FormatName(format) + "_" + label;
  if (!g_filter.empty() && name != g_filter) return;

  const std::uint32_t blocks_wide = (width + 3) / 4;
  const std::uint32_t blocks_high = (height + 3) / 4;
  const std::size_t source_bytes =
      static_cast<std::size_t>(blocks_wide) * blocks_high * EtcBlockBytes(format);
  const std::size_t dest_bytes =
      static_cast<std::size_t>(width) * height * EtcDecodedTexelBytes(format);

  // One buffer per image. Sizing the outer vector by `images * bytes` gives a
  // vector of empty vectors, and then writing into them is a heap overflow --
  // which is exactly the segfault this harness produced before it was fixed.
  std::vector<std::vector<std::uint8_t>> sources(images, std::vector<std::uint8_t>(source_bytes));
  std::vector<std::vector<std::uint8_t>> dests(images, std::vector<std::uint8_t>(dest_bytes));
  BatchWork work{};
  work.workers = workers;
  for (std::uint32_t i = 0; i < images; ++i) {
    Corpus corpus(0x2545f4914f6cdd1dULL ^ (static_cast<std::uint64_t>(i) << 20));
    std::uint8_t* src = sources[i].data();
    corpus.Fill(src, source_bytes);
    for (std::size_t b = 0; b * EtcBlockBytes(format) < source_bytes; ++b) {
      std::uint8_t* block = src + b * EtcBlockBytes(format);
      block[0] = static_cast<std::uint8_t>(b * 31);
      block[1] = static_cast<std::uint8_t>(b * 17);
      block[3] = static_cast<std::uint8_t>(b * 7);
    }
    graphics::EtcDecodeJob job;
    job.format = format;
    job.source = sources[i].data();
    job.source_bytes = source_bytes;
    job.width = width;
    job.height = height;
    job.destination = dests[i].data();
    job.destination_bytes = dest_bytes;
    work.jobs.push_back(job);
  }

  const Timing cpp = TimeTrials(g_iters, g_trials, BatchCpp, &work);
  const Timing rust = TimeTrials(g_iters, g_trials, BatchRust, &work);
  std::printf("  %-22s %8.1f cpp | %8.1f rust | %5.2fx (med %5.2fx) | %u workers, %u images\n",
              name.c_str(), cpp.best_ns / 1000.0, rust.best_ns / 1000.0,
              cpp.best_ns / rust.best_ns, cpp.median_ns / rust.median_ns,
              workers, images);
}


// --------------------------------------------------------- concurrent batches
//
// The C++ pool's documented weakness is that it *declines* when it is already
// busy, and the caller then drains the whole batch itself -- on the thread that
// also has to submit the frame. That only shows up when batches overlap, which
// is the normal case during a load: several uploads in flight at once. A
// single-batch measurement cannot see the difference at all, which is why the
// batched numbers above read 1.00x.

/// One thread's worth of work: the buffers have to outlive the timed section, so
/// they live here rather than in a local that dies at the end of setup.
struct ThreadWork {
  std::vector<std::vector<std::uint8_t>> sources;
  std::vector<std::vector<std::uint8_t>> destinations;
  std::vector<graphics::EtcDecodeJob> jobs;
};

struct ConcurrentWork {
  std::vector<ThreadWork> threads;
  unsigned workers;
};

void* ConcurrentBody(void* context) {
  ConcurrentWork* work = static_cast<ConcurrentWork*>(context);
  std::uint64_t sink = 0;
  for (ThreadWork& thread : work->threads) {
    graphics::DecodeEtcJobs(thread.jobs.data(), thread.jobs.size(),
                            work->workers);
    for (const auto& job : thread.jobs) {
      sink += Checksum(job.destination, job.destination_bytes);
    }
  }
  // Folded into a volatile so the decode cannot be optimised away.
  static volatile std::uint64_t discard = 0;
  discard = discard ^ sink;
  return nullptr;
}

void RunConcurrent(const char* label, EtcFormat format, std::uint32_t width,
                   std::uint32_t height, std::uint32_t per_thread,
                   unsigned threads, unsigned workers) {
  const std::string name =
      std::string("concurrent_") + FormatName(format) + "_" + label;
  if (!g_filter.empty() && name != g_filter) return;

  const std::uint32_t blocks_wide = (width + 3) / 4;
  const std::uint32_t blocks_high = (height + 3) / 4;
  const std::size_t source_bytes = static_cast<std::size_t>(blocks_wide) *
                                   blocks_high * EtcBlockBytes(format);
  const std::size_t dest_bytes = static_cast<std::size_t>(width) * height *
                                 EtcDecodedTexelBytes(format);

  ConcurrentWork work{};
  work.workers = workers;
  work.threads.resize(threads);
  for (unsigned t = 0; t < threads; ++t) {
    ThreadWork& tw = work.threads[t];
    tw.sources.assign(per_thread, std::vector<std::uint8_t>(source_bytes));
    tw.destinations.assign(per_thread, std::vector<std::uint8_t>(dest_bytes));
    for (std::uint32_t i = 0; i < per_thread; ++i) {
      Corpus corpus(0x2545f4914f6cdd1dULL ^
                    (static_cast<std::uint64_t>(t) << 32) ^ i);
      std::uint8_t* src = tw.sources[i].data();
      corpus.Fill(src, source_bytes);
      // Vary the mode selectors so the corpus is not all one ETC2 path.
      for (std::size_t b = 0; b * EtcBlockBytes(format) < source_bytes; ++b) {
        std::uint8_t* block = src + b * EtcBlockBytes(format);
        block[0] = static_cast<std::uint8_t>(b * 31);
        block[1] = static_cast<std::uint8_t>(b * 17);
        block[3] = static_cast<std::uint8_t>(b * 7);
      }
      graphics::EtcDecodeJob job;
      job.format = format;
      job.source = src;
      job.source_bytes = source_bytes;
      job.width = width;
      job.height = height;
      job.destination = tw.destinations[i].data();
      job.destination_bytes = dest_bytes;
      tw.jobs.push_back(job);
    }
  }

  auto run = [&](int rust_pool) {
    graphics::SetRustPoolToggle(rust_pool);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool_threads;
    pool_threads.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
      pool_threads.emplace_back(ConcurrentBody, &work);
    }
    for (auto& th : pool_threads) {
      th.join();
    }
    const auto end = std::chrono::steady_clock::now();
    return static_cast<double>(std::chrono::duration_cast<
                               std::chrono::microseconds>(end - start)
                                   .count()) *
           1000.0;
  };

  std::vector<double> cpp_samples;
  std::vector<double> rust_samples;
  for (int trial = 0; trial < g_trials; ++trial) {
    cpp_samples.push_back(run(0));
    rust_samples.push_back(run(1));
  }
  std::sort(cpp_samples.begin(), cpp_samples.end());
  std::sort(rust_samples.begin(), rust_samples.end());
  std::printf(
      "  %-22s %8.1f cpp | %8.1f rust | %5.2fx (med %5.2fx) | %u threads x "
      "%u imgs\n",
      name.c_str(), cpp_samples.front() / 1000.0, rust_samples.front() / 1000.0,
      cpp_samples.front() / rust_samples.front(),
      cpp_samples[cpp_samples.size() / 2] / rust_samples[rust_samples.size() / 2],
      threads, per_thread);
}


// ------------------------------------------------- kernel microbenchmarks
//
// The whole-image numbers above say which formats cost something but not which
// *part* of them does, and they are dominated by the C++ DecodeRgbBlock, which
// is not ported. These isolate the ported kernels so the time spent in each is
// visible on its own, which is what tells you whether a wider vector unit is
// worth anything.

constexpr std::int32_t kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},   {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},   {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},   {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},   {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},     {-3, -5, -7, -9, 2, 4, 6, 8},
};

inline std::uint64_t ReferenceReadEacBits(const std::uint8_t* b) {
  std::uint64_t bits = 0;
  for (int i = 2; i < 8; ++i) {
    bits = (bits << 8) | b[i];
  }
  return bits;
}

inline int ReferenceEacIndex(std::uint64_t bits, int texel) {
  return static_cast<int>((bits >> (45 - texel * 3)) & 7);
}

struct AlphaWork {
  const std::uint8_t* blocks;
  std::uint32_t count;
  std::uint32_t* texels;
};

void AlphaCpp(void* context) {
  AlphaWork* work = static_cast<AlphaWork*>(context);
  for (std::uint32_t i = 0; i < work->count; ++i) {
    std::uint32_t* cols = work->texels + i * 16;
    const std::uint8_t* block = work->blocks + i * 8;
    const std::uint64_t bits = ReferenceReadEacBits(block);
    const int base = block[0];
    const int mult = block[1] >> 4;
    const auto& table = kEacModifiers[block[1] & 15];
    std::uint8_t palette[8];
    for (int p = 0; p < 8; ++p) {
      palette[p] = static_cast<std::uint8_t>(std::clamp(base + table[p] * mult, 0, 255));
    }
    for (int t = 0; t < 16; ++t) {
      cols[t] = (cols[t] & 0x00FFFFFF) |
                (static_cast<std::uint32_t>(palette[ReferenceEacIndex(bits, t)]) << 24);
    }
  }
}

void AlphaRust(void* context) {
  AlphaWork* work = static_cast<AlphaWork*>(context);
  for (std::uint32_t i = 0; i < work->count; ++i) {
    mocktail_rs_eac_merge_alpha(work->texels + i * 16, work->blocks + i * 8);
  }
}

void RunAlphaMerge(const char* label, std::uint32_t count) {
  const std::string name = std::string("alpha_") + label;
  if (!g_filter.empty() && name != g_filter) return;
  std::vector<std::uint8_t> blocks(count * 8);
  Corpus corpus(0x2545f4914f6cdd1dULL);
  corpus.Fill(blocks.data(), blocks.size());
  std::vector<std::uint32_t> cpp_texels(count * 16, 0x11223300u);
  std::vector<std::uint32_t> rs_texels(cpp_texels);
  AlphaWork cpp{blocks.data(), count, cpp_texels.data()};
  AlphaWork rs{blocks.data(), count, rs_texels.data()};
  const Timing a = TimeTrials(g_iters, g_trials, AlphaCpp, &cpp);
  const Timing b = TimeTrials(g_iters, g_trials, AlphaRust, &rs);
  std::printf("  %-22s %8.1f cpp | %8.1f rust | %5.2fx | %s\n", name.c_str(),
              a.best_ns / 1000.0, b.best_ns / 1000.0, a.best_ns / b.best_ns,
              cpp_texels == rs_texels ? "identical" : "DIFFER!");
}

int channels_of(int format) { return (format == 5 || format == 6) ? 2 : 1; }

struct R11Work {
  const std::uint8_t* blocks;
  std::uint32_t count;
  int format;
  std::uint16_t* out;
};

inline std::uint16_t ReferenceR11Modifier(const std::uint8_t* b, int multiplier,
                                          const int* table, int index, bool is_signed) {
  const int modifier = table[index];
  const int scaled = multiplier != 0 ? modifier * multiplier * 8 : modifier;
  if (!is_signed) {
    const int value = std::clamp(b[0] * 8 + 4 + scaled, 0, 2047);
    return static_cast<std::uint16_t>((value << 5) | (value >> 6));
  }
  const int base = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
  const int value = std::clamp(base * 8 + scaled, -1023, 1023);
  const int magnitude = value < 0 ? -value : value;
  const int expanded = (magnitude << 5) | (magnitude >> 5);
  return static_cast<std::uint16_t>(
      static_cast<std::int16_t>(value < 0 ? -expanded : expanded));
}

void R11Cpp(void* context) {
  R11Work* work = static_cast<R11Work*>(context);
  const bool is_signed = work->format == 4 || work->format == 6;
  const int channels = (work->format == 5 || work->format == 6) ? 2 : 1;
  for (std::uint32_t i = 0; i < work->count; ++i) {
    const std::uint8_t* block = work->blocks + i * 16;
    std::uint64_t bits[2] = {0, 0};
    std::uint16_t palette[2][8];
    for (int c = 0; c < channels; ++c) {
      const std::uint8_t* cb = block + c * 8;
      bits[c] = ReferenceReadEacBits(cb);
      const int mult = cb[1] >> 4;
      const int* table = kEacModifiers[cb[1] & 15];
      for (int p = 0; p < 8; ++p) {
        palette[c][p] = ReferenceR11Modifier(cb, mult, table, p, is_signed);
      }
    }
    std::uint16_t* out = work->out + i * 16 * channels;
    for (int t = 0; t < 16; ++t) {
      for (int c = 0; c < channels; ++c) {
        out[t * channels + c] = palette[c][ReferenceEacIndex(bits[c], t)];
      }
    }
  }
}

void R11Rust(void* context) {
  R11Work* work = static_cast<R11Work*>(context);
  for (std::uint32_t i = 0; i < work->count; ++i) {
    mocktail_rs_decode_r11_block(work->format, work->blocks + i * 16, 16,
                                 work->out + i * 16 * channels_of(work->format));
  }
}

void RunR11Block(const char* label, int format, std::uint32_t count) {
  const std::string name = std::string("r11_") +
                          FormatName(static_cast<EtcFormat>(format)) + "_" + label;
  if (!g_filter.empty() && name != g_filter) return;
  const int channels = channels_of(format);
  std::vector<std::uint8_t> blocks(count * 16);
  Corpus corpus(0x2545f4914f6cdd1dULL);
  corpus.Fill(blocks.data(), blocks.size());
  std::vector<std::uint16_t> cpp_out(count * 16 * channels);
  std::vector<std::uint16_t> rs_out(cpp_out.size());
  R11Work cpp{blocks.data(), count, format, cpp_out.data()};
  R11Work rs{blocks.data(), count, format, rs_out.data()};
  const Timing a = TimeTrials(g_iters, g_trials, R11Cpp, &cpp);
  const Timing b = TimeTrials(g_iters, g_trials, R11Rust, &rs);
  std::printf("  %-22s %8.1f cpp | %8.1f rust | %5.2fx | %s\n", name.c_str(),
              a.best_ns / 1000.0, b.best_ns / 1000.0, a.best_ns / b.best_ns,
              cpp_out == rs_out ? "identical" : "DIFFER!");
}

}  // namespace

int main(int argc, char** argv) {
  // Chosen so a full sweep takes a few seconds on a modest machine.
  g_iters = 120;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
      g_iters = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      g_filter = argv[i + 1];
      ++i;
    }
  }

  std::printf("kernel_benchmark: %d iterations, rust kernels %s, simd %d\n",
              g_iters, mocktail_rs_available() ? "linked" : "absent",
              mocktail_rs_simd_level());

  std::printf("\nResampleRgba (box filter, the small-texture-upscale path)\n");
  // The default upscale is 4, applied to textures of 64px or smaller.
  RunResample("resample_64x64_x1", 64, 64, 1);
  RunResample("resample_64x64_x2", 64, 64, 2);
  RunResample("resample_64x64_x4", 64, 64, 4);
  RunResample("resample_32x32_x4", 32, 32, 4);
  RunResample("resample_128x128_x2", 128, 128, 2);
  RunResample("resample_256x256_x4", 256, 256, 4);

  std::printf("\nETC2/EAC decode (the texture upload path)\n");
  // 128x128 is a mid-size texture; small_texture_upscale targets 64px or less,
  // so the sub-64 cases are the ones that decide load-time cost.
  RunEtc("128x128", EtcFormat::kEtc2Rgb8, 128, 128);
  RunEtc("128x128", EtcFormat::kEtc2Rgba8, 128, 128);
  RunEtc("64x64", EtcFormat::kEtc2Rgb8, 64, 64);
  RunEtc("64x64", EtcFormat::kEtc2Rgba8, 64, 64);
  RunEtc("64x64", EtcFormat::kEacR11, 64, 64);
  RunEtc("64x64", EtcFormat::kEacRg11, 64, 64);
  RunEtc("256x256", EtcFormat::kEacRg11, 256, 256);

  std::printf("\nBatched decode (DecodeEtcJobs, the pool path)\n");
  // One upload of 32 128x128 textures is what a place with a lot of geometry
  // produces in a single flush.
  RunBatch("256x128x32", EtcFormat::kEtc2Rgb8, 128, 128, 32, 4);
  RunBatch("256x128x32", EtcFormat::kEtc2Rgba8, 128, 128, 32, 4);
  RunBatch("64x64x128", EtcFormat::kEtc2Rgb8, 64, 64, 128, 4);
  RunBatch("256x128x32", EtcFormat::kEtc2Rgb8, 128, 128, 32, 8);

  std::printf("\nConcurrent batches (the pool's decline-vs-steal case)\n");
  RunConcurrent("4t_128x128x16", EtcFormat::kEtc2Rgb8, 128, 128, 16, 4, 4);
  RunConcurrent("8t_64x64x32", EtcFormat::kEtc2Rgb8, 64, 64, 32, 8, 4);
  RunConcurrent("4t_128x128x16", EtcFormat::kEtc2Rgba8, 128, 128, 16, 4, 4);

  std::printf("\nPorted kernels in isolation\n");
  RunAlphaMerge("16384blocks", 16384);
  RunR11Block("16384blocks", 3, 16384);
  RunR11Block("16384blocks", 5, 16384);
  RunR11Block("16384blocks", 6, 16384);

  std::printf("\n");
  return 0;
}
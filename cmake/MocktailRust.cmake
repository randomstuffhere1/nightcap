# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.
#
# Rust kernel workspace integration.
#
# The workspace at ${CMAKE_SOURCE_DIR}/rust produces libmocktail_rs.a, a
# staticlib that links into both the shared stub_vulkan target and the
# standalone test executables, so a cdylib would need RPATH work everywhere and
# a staticlib needs none.
#
# Two deliberate choices worth keeping:
#
#   * CARGO_TARGET_DIR is redirected into ${CMAKE_BINARY_DIR}. Cargo defaults to
#     ./target, which is not covered by .gitignore and which `make clean` does
#     not remove. Pointing it at the build tree keeps it ignored and cleaned.
#   * The archive cargo emits carries std's debuginfo (~2.6 MiB, because Rust's
#     own dist std is built with it) and .llvmbc sections (~1.5 MiB) that are
#     only used for cross-language LTO. Neither is used here -- the C++ side is
#     -fno-exceptions and Rust is panic=abort, and cross-language LTO is off --
#     so both are stripped out of the archive after the cargo run. Together
#     that takes a ~7.8 MiB archive down to ~1.4 MiB.

include_guard(GLOBAL)

get_filename_component(MOCKTAIL_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

option(MOCKTAIL_BUILD_RUST_KERNELS
  "Build the Rust kernel library with cargo instead of using a prebuilt archive" ON
)

set(MOCKTAIL_RUST_WORKSPACE "${MOCKTAIL_ROOT}/rust")
set(MOCKTAIL_RUST_INCLUDE_DIR "${MOCKTAIL_ROOT}/include")

# A prebuilt archive wins over building one, because it is the only way to get
# Rust kernels into a build environment that has no cargo: a distro package that
# ships one, or a cross build whose host has no matching Rust target.
set(MOCKTAIL_RUST_LIB "${MOCKTAIL_RUST_PREBUILT}")
if(MOCKTAIL_RUST_PREBUILT AND NOT EXISTS "${MOCKTAIL_RUST_PREBUILT}")
  message(FATAL_ERROR
    "MOCKTAIL_RUST_PREBUILT=${MOCKTAIL_RUST_PREBUILT} does not exist.")
endif()

if(NOT MOCKTAIL_RUST_LIB)
  if(NOT MOCKTAIL_BUILD_RUST_KERNELS)
    message(FATAL_ERROR
      "The Rust kernels are not being built and no prebuilt archive was given.\n"
      "Set -DMOCKTAIL_RUST_PREBUILT=/path/to/libmocktail_rs.a, or leave\n"
      "-DMOCKTAIL_BUILD_RUST_KERNELS=ON to build ${MOCKTAIL_RUST_WORKSPACE}\n"
      "(needs a Rust toolchain; see README, Building).")
  endif()

  find_program(CARGO_EXECUTABLE cargo)
  if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
      "cargo was not found, and the Rust kernels have to be built.\n"
      "Install a Rust toolchain (see README, Building), or set\n"
      "-DMOCKTAIL_RUST_PREBUILT=/path/to/libmocktail_rs.a to use a prebuilt "
      "archive.")
  endif()

  # Maps the CMake release flag onto a cargo invocation. Without --target the
  # archive lands in ${target_dir}/release; with it, in
  # ${target_dir}/<triple>/release, so MOCKTAIL_RUST_LIB below has to match.
  set(MOCKTAIL_RUST_PROFILE_RELEASE OFF)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MOCKTAIL_RUST_PROFILE_RELEASE ON)
  endif()

  set(MOCKTAIL_RUST_TARGET_TRIPLE "${MOCKTAIL_RUST_TARGET}")
  if(MOCKTAIL_RUST_TARGET_TRIPLE STREQUAL "" AND CMAKE_CROSSCOMPILING)
    set(MOCKTAIL_RUST_TARGET_TRIPLE "${CMAKE_C_COMPILER_TARGET}")
  endif()

  # --locked keeps Cargo.lock authoritative, and --offline makes the build
  # honour the vendored crates under rust/vendor without reaching the network.
  set(MOCKTAIL_RUST_CARGO_ARGS build --locked --offline)
  if(NOT MOCKTAIL_RUST_TARGET_TRIPLE STREQUAL "")
    list(APPEND MOCKTAIL_RUST_CARGO_ARGS "--target=${MOCKTAIL_RUST_TARGET_TRIPLE}")
  endif()
  if(MOCKTAIL_RUST_PROFILE_RELEASE)
    list(APPEND MOCKTAIL_RUST_CARGO_ARGS --release)
  endif()

  set(MOCKTAIL_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
  if(MOCKTAIL_RUST_PROFILE_RELEASE)
    set(MOCKTAIL_RUST_PROFILE release)
  else()
    set(MOCKTAIL_RUST_PROFILE debug)
  endif()
  set(MOCKTAIL_RUST_LIB
    "${MOCKTAIL_RUST_TARGET_DIR}/${MOCKTAIL_RUST_PROFILE}/libmocktail_rs.a"
  )
  if(NOT MOCKTAIL_RUST_TARGET_TRIPLE STREQUAL "")
    set(MOCKTAIL_RUST_LIB
      "${MOCKTAIL_RUST_TARGET_DIR}/${MOCKTAIL_RUST_TARGET_TRIPLE}/${MOCKTAIL_RUST_PROFILE}/libmocktail_rs.a"
    )
  endif()

  # CONFIGURE_DEPENDS re-runs CMake when a matching file appears or disappears,
  # so adding a new module does not silently skip it until the next hand-run.
  file(GLOB_RECURSE MOCKTAIL_RUST_SOURCES CONFIGURE_DEPENDS
    "${MOCKTAIL_RUST_WORKSPACE}/Cargo.toml"
    "${MOCKTAIL_RUST_WORKSPACE}/Cargo.lock"
    "${MOCKTAIL_RUST_WORKSPACE}/crates/*/Cargo.toml"
    "${MOCKTAIL_RUST_WORKSPACE}/crates/*/src/*.rs"
    "${MOCKTAIL_RUST_WORKSPACE}/.cargo/config.toml"
  )

  find_program(MOCKTAIL_OBJCOPY objcopy)
  find_program(MOCKTAIL_STRIP strip)
  if(MOCKTAIL_OBJCOPY AND MOCKTAIL_STRIP AND NOT MOCKTAIL_FORCE_DEBUG_SYMBOLS)
    set(MOCKTAIL_RUST_STRIP_BITCODE
      ${MOCKTAIL_OBJCOPY} --remove-section=.llvmbc "${MOCKTAIL_RUST_LIB}")
    set(MOCKTAIL_RUST_STRIP_DEBUG
      ${MOCKTAIL_STRIP} --strip-debug "${MOCKTAIL_RUST_LIB}")
    set(MOCKTAIL_RUST_STRIP_LABEL " and stripping")
  else()
    set(MOCKTAIL_RUST_STRIP_BITCODE ${CMAKE_COMMAND} -E true)
    set(MOCKTAIL_RUST_STRIP_DEBUG ${CMAKE_COMMAND} -E true)
    set(MOCKTAIL_RUST_STRIP_LABEL "")
  endif()

  add_custom_command(
    OUTPUT "${MOCKTAIL_RUST_LIB}"
    # The working directory is the workspace, so cargo finds
    # rust/.cargo/config.toml (vendored sources, rustflags) rather than walking
    # up from the build tree.
    COMMAND ${CMAKE_COMMAND} -E env
              "CARGO_TARGET_DIR=${MOCKTAIL_RUST_TARGET_DIR}"
              "${CARGO_EXECUTABLE}" ${MOCKTAIL_RUST_CARGO_ARGS}
    # Strip in the same command, in place, so the ordering is unconditional and
    # build/rust-target itself stays small.
    COMMAND ${MOCKTAIL_RUST_STRIP_BITCODE}
    COMMAND ${MOCKTAIL_RUST_STRIP_DEBUG}
    WORKING_DIRECTORY "${MOCKTAIL_RUST_WORKSPACE}"
    DEPENDS ${MOCKTAIL_RUST_SOURCES}
    COMMENT "Building${MOCKTAIL_RUST_STRIP_LABEL} Rust kernels (${MOCKTAIL_RUST_PROFILE})"
    VERBATIM
  )

  add_custom_target(mocktail_rust_kernel_archive DEPENDS "${MOCKTAIL_RUST_LIB}")
endif()

add_library(mocktail_rust_kernels INTERFACE)
if(TARGET mocktail_rust_kernel_archive)
  # Only exists when this build produced the archive; a prebuilt one has no
  # build-order dependency to hang on it.
  add_dependencies(mocktail_rust_kernels mocktail_rust_kernel_archive)
endif()
target_include_directories(mocktail_rust_kernels INTERFACE
  "${MOCKTAIL_RUST_INCLUDE_DIR}"
)
target_link_libraries(mocktail_rust_kernels INTERFACE
  "${MOCKTAIL_RUST_LIB}"
  Threads::Threads
  ${CMAKE_DL_LIBS}
)
add_library(Mocktail::RustKernels ALIAS mocktail_rust_kernels)

if(MOCKTAIL_RUST_PREBUILT)
  message(STATUS "Rust kernels: prebuilt ${MOCKTAIL_RUST_PREBUILT}")
else()
  message(STATUS "Rust kernels: ${MOCKTAIL_RUST_PROFILE} via ${CARGO_EXECUTABLE}")
endif()

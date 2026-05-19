cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)
add_library(audiosync SHARED native-lib.c)

target_compile_options(audiosync PRIVATE
    -std=c11

    # ── Optimisation level ────────────────────────────────────────────────────
    -Ofast                              # O3 + -ffast-math + extra aggressive opts

    # ── Instruction-set / micro-arch ─────────────────────────────────────────
    # armv8.2-a is the baseline for all Android 9+ (minSdk 28) mid/high-end SoCs.
    # +simd enables NEON; +dotprod enables SDOT/UDOT; +fp16 enables FP16 NEON.
    -march=armv8.2-a+simd+fp16+dotprod+crypto
    -mtune=cortex-a76                   # Tune scheduling for Cortex-A76 class cores

    # ── Link-time optimisation ───────────────────────────────────────────────
    -flto=thin                          # Thin LTO: fast cross-TU inlining via cached summaries

    # ── Loop & code-gen transforms ───────────────────────────────────────────
    -funroll-loops                      # Unroll inner loops to reduce branch overhead
    -fomit-frame-pointer                # Frees fp register in hot paths (frame-pointer not needed)
    -fno-plt                            # Avoid PLT trampolines on shared-lib calls
    -fno-semantic-interposition         # Allow intra-.so inlining (Clang ≥ 7)
    -fno-common                         # No tentative definitions; enables better DCE

    # ── Floating-point ───────────────────────────────────────────────────────
    -ffast-math                         # No NaN/Inf guards, allows FP reassociation
    -fstrict-aliasing                   # Assume no pointer aliasing for vectorisation

    # ── Safety-overhead removal ──────────────────────────────────────────────
    -fno-stack-protector                # Remove per-frame stack-canary checks
    -fno-unwind-tables                  # Drop .ARM.exidx unwind entries
    -fno-asynchronous-unwind-tables     # Drop DWARF CFI async-unwind tables
    -fno-exceptions                     # No C++ exception machinery (already C; belt-and-braces)

    # ── Dead-code / icache size ──────────────────────────────────────────────
    -ffunction-sections                 # One ELF section per function (enables --gc-sections)
    -fdata-sections                     # One ELF section per variable
    -fvisibility=hidden                 # Hide all symbols by default; improves LTO & strip

    # ── Misc ─────────────────────────────────────────────────────────────────
    -DNDEBUG                            # Disable assert() in minimp3 and other libs
)

# Activate CMake-level IPO (adds -flto to all targets in case of subdirectories)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

target_link_options(audiosync PRIVATE
    -flto=thin                  # Thin LTO at link stage (matches compile stage)
    -Wl,--gc-sections           # Remove dead sections (pairs with -ffunction/data-sections)
    -Wl,-O3                     # Linker-level optimisation passes (lld)
    -Wl,--as-needed             # Drop libraries with no referenced symbols
    -Wl,--icf=all               # Identical-code folding: merge duplicate functions
    -Wl,-z,norelro              # Skip RELRO remapping (removes startup latency)
)

target_link_libraries(audiosync log aaudio)

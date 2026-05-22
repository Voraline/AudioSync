cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)
add_library(audiosync SHARED native-lib.c)

target_compile_options(audiosync PRIVATE
    -std=c11

    # ── Optimisation level ────────────────────────────────────────────────────
    # -O3 instead of -Ofast: -Ofast implies -ffast-math which enables
    # -fassociative-math, allowing the compiler to reorder the near-cancellation
    # subtraction in AudioCallback (two large timestamps ~10^11 µs differing by
    # only ~10^5 µs). Reordering that loses all precision in ExactF, shifting
    # playback by potentially hundreds of milliseconds. -O3 keeps every other
    # scalar and vectorisation optimisation without touching FP semantics.
    -O3

    # ── Instruction-set / micro-arch ─────────────────────────────────────────
    # armv8.2-a is the baseline for all Android 9+ (minSdk 28) mid/high-end SoCs.
    # +simd enables NEON; +dotprod enables SDOT/UDOT; +fp16 enables FP16 NEON.
    # NOTE: +fp16 is kept for _Float16 data-type support (minimp3 SIMD paths)
    # but WITHOUT -ffast-math the compiler will NOT auto-demote float32 operands
    # to fp16, so AudioCallback's Catmull-Rom interpolation stays in float32.
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
    # -ffast-math is REMOVED. It was the primary sync-breaker via three sub-flags:
    #   -fassociative-math  : allowed reordering (CurrentServerTimeUs - TargetFirePcUs),
    #                         causing catastrophic cancellation — both values are ~10^11 µs
    #                         but differ by only ~10^5 µs, so reordering loses all precision
    #                         in ExactF and can shift playback start by 100s of milliseconds.
    #   -ffp-contract=fast  : fused multiply-add across statement boundaries, changing
    #                         ExactF rounding non-deterministically between compiler runs.
    #   -freciprocal-math   : replaced /1000000.0 with *approx(1e-6), introducing a
    #                         systematic bias in the computed sample index.
    #   +fp16 + -ffast-math : allowed Clang to auto-vectorize Catmull-Rom interpolation
    #                         using FP16 NEON lanes, quantizing sub-sample position to
    #                         ~22 µs steps (FP16 has 10-bit mantissa vs float32's 23-bit).
    #
    # -fno-math-errno: safe to keep — only removes errno side-effects from libm
    #   calls (sqrtf, sqrt), has zero effect on FP precision or operation ordering.
    -fno-math-errno
    # -ffp-contract=on: allows FMA only within a single expression (IEEE 754-2008 §5.4.1
    #   compliant). The Catmull-Rom Horner evaluation gets fused for better precision;
    #   the timestamp arithmetic above the ExactF line is never touched.
    -ffp-contract=on
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
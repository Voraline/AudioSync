cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)
add_library(audiosync SHARED native-lib.c)

target_compile_options(audiosync PRIVATE
    -std=c11

    # ── Optimisation level ────────────────────────────────────────────────────
    # IMPORTANT: must be -O3, NOT -Ofast.
    # -Ofast implies -ffast-math which permits the compiler to reassociate
    # floating-point additions. The sigma-filter loops in DoClockSync and
    # DoRollingProbe accumulate clock offsets into Sum/Var; reordering those
    # additions changes Mean and Sigma, which changes which samples survive the
    # 0.8-sigma rejection gate, which changes FinalOffset / ClockOffsetUs.
    # That is a direct path to clients drifting out of sync.
    -O3

    # ── Instruction-set / micro-arch ─────────────────────────────────────────
    # armv8.2-a is the baseline for all Android 9+ mid/high-end SoCs (minSdk 28).
    # +simd enables NEON 128-bit vector lanes used by the Catmull-Rom audio loop.
    # +dotprod enables SDOT/UDOT for any future integer-accumulate paths.
    # +crypto enables AES/SHA hardware (unused now but zero cost to enable).
    # fp16 removed: enabling it lets the compiler use half-precision intermediates
    # in autovectorised paths, which can corrupt the float timing calculations.
    -march=armv8.2-a+simd+dotprod+crypto
    -mtune=cortex-a76   # Tune instruction scheduling for Cortex-A76 class cores
                        # (Snapdragon 855/865, Exynos 990, etc.)

    # ── Link-time optimisation ───────────────────────────────────────────────
    # Thin LTO performs cross-translation-unit inlining using cached per-file
    # summaries rather than re-reading all bitcode; much faster than full LTO
    # while capturing most of the cross-TU optimisation benefit.
    -flto=thin

    # ── Loop & code-gen transforms ───────────────────────────────────────────
    -funroll-loops          # Unroll inner loops to reduce branch overhead in the
                            # Catmull-Rom interpolation and sigma accumulation loops
    -fomit-frame-pointer    # Frees the frame-pointer register in hot-path functions;
                            # safe because unwind tables are dropped below
    -fno-plt                # Avoid PLT trampolines on shared-library calls — direct
                            # GOT dispatch is faster and removes one branch per call
    -fno-semantic-interposition # Allow the linker to inline calls within the .so
                                # without going through the PLT (Clang 7+)
    -fno-common             # No tentative (common) definitions; lets the linker
                            # eliminate truly unreferenced variables via --gc-sections

    # ── Floating-point ───────────────────────────────────────────────────────
    # -ffp-contract=off: disables fused multiply-add contraction.
    # A contraction like (a*b)+c → fma(a,b,c) changes the rounding of the
    # intermediate result. In ExactF = (CurrentServerTimeUs - TargetFirePcUs)
    # * PcmSampleRate / 1000000.0 a contracted form shifts the exact fractional
    # sample index, directly moving when audio fires relative to the server clock.
    -ffp-contract=off

    # -fexcess-precision=standard: forbids the compiler from holding any float or
    # double in a wider register than its declared type between operations.
    # Without this Clang may keep intermediates at full register width across
    # loop iterations, producing values that differ from strict IEEE 754 arithmetic.
    -fexcess-precision=standard

    # -fno-unsafe-math-optimizations: blocks reassociation, reciprocal substitution,
    # and similar transforms that are algebraically valid but change results in
    # finite-precision arithmetic. With -O3 + -flto=thin the compiler would
    # otherwise freely apply these across inlined functions at link time, which
    # would affect the Mean/Sigma computation in DoClockSync and DoRollingProbe.
    -fno-unsafe-math-optimizations

    # ── Safety-overhead removal ──────────────────────────────────────────────
    -fno-stack-protector            # Remove per-frame stack-canary checks
    -fno-unwind-tables              # Drop .ARM.exidx unwind entries
    -fno-asynchronous-unwind-tables # Drop DWARF CFI async-unwind tables
    -fno-exceptions                 # No C++ exception machinery (belt-and-braces for C)

    # ── Dead-code / icache footprint ─────────────────────────────────────────
    -ffunction-sections # One ELF section per function — lets --gc-sections at link
                        # time discard every function not reachable from an entry point
    -fdata-sections     # Same for variables
    -fvisibility=hidden # Hide all symbols by default; improves LTO inlining decisions
                        # and allows --strip-all to remove the full symbol table

    # ── Misc ─────────────────────────────────────────────────────────────────
    -DNDEBUG            # Disable assert() in minimp3 and other debug guards
)

# Tell CMake to apply IPO/LTO to any subdirectory targets as well.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

target_link_options(audiosync PRIVATE
    -flto=thin          # Thin LTO at link stage — must match the compile-stage flag
    -Wl,--gc-sections   # Discard ELF sections with no live references (pairs with
                        # -ffunction-sections / -fdata-sections above)
    -Wl,-O3             # Run lld's own optimisation passes at link time
    -Wl,--as-needed     # Drop any library that contributes no referenced symbols
    -Wl,--icf=all       # Identical-code folding: merge duplicate function bodies
                        # into a single copy to shrink the icache footprint
    -Wl,-z,norelro      # Skip RELRO segment remapping on startup — removes the
                        # mprotect() call that would otherwise add latency at launch
)

target_link_libraries(audiosync log aaudio)
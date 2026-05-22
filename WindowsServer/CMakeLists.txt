cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)
add_executable(AudioSyncServer main.c)

if(MSVC)
    # Strip /RTC1 (runtime checks) and /Od (no-optimise) that CMake injects into
    # Debug builds — they would silently override the /O2 below if left in.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od"   "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

    # Remove the default /GS (stack security cookie) CMake adds globally.
    # We disable it explicitly below; leaving both would cause a conflict.
    string(REPLACE "/GS"   "" CMAKE_C_FLAGS       "${CMAKE_C_FLAGS}")

    target_compile_options(AudioSyncServer PRIVATE

        # ── Optimisation level ────────────────────────────────────────────────
        /O2         # Maximum speed preset: enables /Oi /Ot /Oy /Ob2 /GF /Gs
        /Ob3        # Aggressive inlining beyond /Ob2 — inlines larger functions
                    # that /O2 would leave as calls (VS 2019 16.3+)
        /Ot         # Favour execution speed over code size when a trade-off exists
        /Oi         # Replace eligible calls (memcpy, sqrt, etc.) with compiler intrinsics
        /Oy         # Omit frame-pointer — frees one register in every hot-path function
        /Gy         # Emit each function as a separate COMDAT section so /OPT:REF
                    # can strip unreferenced functions at link time
        /Gw         # Emit each global variable in its own COMDAT section so /OPT:REF
                    # can strip unreferenced globals at link time — the data-side
                    # equivalent of /Gy. No floating-point effect: purely a linker
                    # visibility change to static storage, not arithmetic.
        /GL         # Whole-program optimisation: defers final code-gen to the linker
                    # so it can inline and optimise across translation-unit boundaries
                    # (prerequisite for /LTCG)

        # ── Instruction-set ───────────────────────────────────────────────────
        /arch:AVX2  # Emit AVX2 + FMA3 instructions; the server runs on a known
                    # desktop/laptop so AVX2 is safe and widens the SIMD registers
                    # used by any auto-vectorised loops

        # ── Floating-point ───────────────────────────────────────────────────
        # IMPORTANT: must be /fp:precise, NOT /fp:fast.
        # /fp:fast allows the compiler to reassociate floating-point additions,
        # which changes the result of the sigma-filter accumulation loops in
        # DoClockSync and DoRollingProbe. Those loops compute Mean and Sigma
        # that gate which clock-offset samples survive; a different Sigma means
        # a different FinalOffset written to ClockOffsetUs, which desycs clients.
        # /fp:precise preserves IEEE 754 order-of-operations while still allowing
        # most speed optimisations (contraction off by default under /fp:precise).
        /fp:precise

        # ── Security overhead removal ─────────────────────────────────────────
        /GS-        # Disable per-frame stack-canary — removes the hidden load/store
                    # and comparison that /GS inserts at every function entry/exit
        /sdl-       # Disable extra SDL security diagnostics (redundant in release)

        # ── Misc ─────────────────────────────────────────────────────────────
        /favor:AMD64    # Tune instruction scheduling for 64-bit AMD/Intel pipelines
        /GA             # Optimise thread-local storage access for Windows executables;
                        # emits a direct FS:/GS: segment reference instead of a helper
                        # call. No floating-point effect: affects only TLS pointer load,
                        # not arithmetic.
        /DNDEBUG        # Disable assert() in minimp3 and any other debug guards
    )

    target_link_options(AudioSyncServer PRIVATE
        /LTCG           # Link-time code generation — works with /GL to perform
                        # cross-TU inlining and constant propagation at link time
        /OPT:REF        # Remove every function and data section not reachable from
                        # an entry point (pairs with /Gy at compile time)
        /OPT:ICF        # Fold identical COMDAT sections — merges duplicate functions
                        # into one copy, shrinking the icache footprint
        /INCREMENTAL:NO # Incremental linking is incompatible with /LTCG; disabling
                        # it avoids a linker warning and ensures a clean full link
    )
endif()
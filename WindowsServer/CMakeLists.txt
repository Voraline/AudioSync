cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)
add_executable(AudioSyncServer main.c)
if(MSVC)
    # Strip debug-only flags that hurt release performance
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od"   "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    # Remove the default /GS (buffer-security-check) injected by CMake
    string(REPLACE "/GS"   "" CMAKE_C_FLAGS       "${CMAKE_C_FLAGS}")

    target_compile_options(AudioSyncServer PRIVATE
        /O2             # Maximum speed: enables /Oi /Ot /Oy /Ob2 /GF /Gs
        /Ob3            # Aggressive inlining beyond /Ob2 (VS 2019 16.3+)
        /Ot             # Favor execution speed over code size
        /Oi             # Replace calls with compiler intrinsics where possible
        /Oy             # Omit frame-pointer (frees a register in hot paths)
        /Gy             # Function-level linking (enables /OPT:REF dead-code removal)
        /GL             # Whole-program optimization (prerequisite for /LTCG)
        /arch:AVX2      # Emit AVX2 / FMA3 instructions for vectorised audio math
        /fp:fast        # Aggressive FP: allows reassociation, no NaN/inf guards
        /GS-            # Disable stack-buffer security cookie (removes per-call overhead)
        /sdl-           # Disable extra SDL security diagnostics
        /favor:AMD64    # Tune instruction selection for 64-bit AMD/Intel
        /DNDEBUG        # Silence assert() and other debug-build guards
    )
    target_link_options(AudioSyncServer PRIVATE
        /LTCG               # Link-time code generation (cross-TU inlining & opt)
        /OPT:REF            # Remove unreferenced functions and data sections
        /OPT:ICF            # Fold identical COMDAT sections (smaller icache footprint)
        /INCREMENTAL:NO     # Disable incremental linking (incompatible with /LTCG)
    )
endif()

include(CheckIPOSupported)

# PERF-PGO: MSVC profile-guided link of the runtime + entry runner (objects are already /GL). OFF = normal build, GEN = instrumented link (run it, then
# pgosweep ps2EntryRunner.exe), USE = optimised link with the collected ps2EntryRunner.pgd. Only the link changes, nothing is recompiled.
set(PS2X_PGO "OFF" CACHE STRING "MSVC PGO mode: OFF, GEN or USE")

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Zi # PDB symbols
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_libraries(${TargetName} PRIVATE dbghelp)
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /DEBUG # Generate PDB
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                    $<$<STREQUAL:${PS2X_PGO},GEN>:/FASTGENPROFILE> # PERF-PGO: /GENPROFILE (full probes) was too slow to play to the hub
                    $<$<STREQUAL:${PS2X_PGO},USE>:/USEPROFILE> # PERF-PGO
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()
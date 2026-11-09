include(CheckIPOSupported)

# PERF-PGO: MSVC profile-guided link of the runtime + entry runner (objects are already /GL). OFF = normal build, GEN = instrumented link (run it, then
# pgosweep ps2EntryRunner.exe), USE = optimised link with the collected ps2EntryRunner.pgd. Only the link changes, nothing is recompiled.
set(PS2X_PGO "OFF" CACHE STRING "MSVC PGO mode: OFF, GEN or USE")
option(PS2X_DEV_BUILD "Fast dev build: no /GL, no LTCG, incremental link (A/B tests only, not acceptance)" OFF)
if(PS2X_DEV_BUILD AND NOT PS2X_PGO STREQUAL "OFF")
    message(FATAL_ERROR "PS2X_DEV_BUILD=ON needs PS2X_PGO=OFF")
endif()

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
                $<$<NOT:$<BOOL:${PS2X_DEV_BUILD}>>:/GL> # whole program opt
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
                    $<$<NOT:$<BOOL:${PS2X_DEV_BUILD}>>:/LTCG> # link-time code generation
                    $<$<NOT:$<BOOL:${PS2X_DEV_BUILD}>>:/OPT:REF> # remove unreferenced
                    $<$<NOT:$<BOOL:${PS2X_DEV_BUILD}>>:/OPT:ICF> # fold identical COMDATs
                    $<$<BOOL:${PS2X_DEV_BUILD}>:/INCREMENTAL>
                    $<$<BOOL:${PS2X_DEV_BUILD}>:/OPT:NOREF>
                    $<$<BOOL:${PS2X_DEV_BUILD}>:/OPT:NOICF>
                    $<$<STREQUAL:${PS2X_PGO},GEN>:/FASTGENPROFILE> # PERF-PGO: /GENPROFILE (full probes) was too slow to play to the hub
                    $<$<STREQUAL:${PS2X_PGO},USE>:/USEPROFILE:PGD=$<TARGET_FILE_DIR:${TargetName}>/ps2EntryRunner_pgogen.pgd> # PERF-PGO
                >
            )
            # PERF-PGO safeguard: the instrumented and the optimised exe get their own file names, so a PGO step can never overwrite the normal
            # ps2EntryRunner.exe (and ninja links the new name without having to delete anything)
            if(TargetName STREQUAL "ps2EntryRunner")
                if(PS2X_DEV_BUILD)
                    set_target_properties(${TargetName} PROPERTIES OUTPUT_NAME "ps2EntryRunner_dev")
                elseif(PS2X_PGO STREQUAL "GEN")
                    set_target_properties(${TargetName} PROPERTIES OUTPUT_NAME "ps2EntryRunner_pgogen")
                elseif(PS2X_PGO STREQUAL "USE")
                    set_target_properties(${TargetName} PROPERTIES OUTPUT_NAME "ps2EntryRunner_pgo")
                endif()
            endif()
        endif()
    endif()

    if(NOT PS2X_DEV_BUILD)
        if(IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
        endif()
    endif()
endfunction()
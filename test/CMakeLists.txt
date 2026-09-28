# Unit tests, split between core/ and light/ to mirror src/.
# A test stays in the subfolder whose domain its primary @module belongs to;
# cross-domain references travel through the @also list, not the directory.
add_executable(mm_tests
    # core/
    unit/core/unit_PartitionTables.cpp
    unit/core/unit_SpscRing.cpp
    unit/core/unit_AnalogService.cpp
    unit/core/unit_AudioCapture.cpp
    unit/core/unit_platform_audiofft.cpp
    unit/core/unit_Buffer.cpp
    unit/core/unit_ScratchBuffer.cpp
    unit/core/unit_Oscillators.cpp
    unit/core/unit_ActiveInstance.cpp
    unit/core/unit_Color.cpp
    unit/core/unit_math8.cpp
    unit/core/unit_math16.cpp
    unit/core/unit_noise.cpp
    unit/core/unit_fields.cpp
    unit/core/unit_crc.cpp
    unit/core/unit_AudioService_sync.cpp
    unit/core/unit_ButtonService.cpp
    unit/core/unit_MoonLiveService.cpp
    unit/core/unit_InfraredService.cpp
    unit/core/unit_TcpConnect.cpp
    unit/core/unit_MqttPacket.cpp
    unit/core/unit_MqttModule.cpp
    unit/core/unit_FileManagerModule.cpp
    unit/core/unit_Control_apply_absent_key.cpp
    unit/core/unit_Control_int32.cpp
    unit/core/unit_Control_filepath.cpp
    unit/core/unit_Control_list.cpp
    unit/core/unit_DeviceIdentify.cpp
    unit/core/unit_DevicesModule_ageout.cpp
    unit/core/unit_DevicesModule_discovery.cpp
    unit/core/unit_DevicesModule_hue.cpp
    unit/core/unit_TasksModule.cpp
    unit/core/unit_PinsModule.cpp
    unit/core/unit_WledPacket.cpp
    unit/core/unit_FilesystemModule_persistence.cpp
    unit/core/unit_ControlModule.cpp
    unit/core/unit_FilesystemModule_subtree.cpp
    unit/core/unit_FirmwareUpdateModule.cpp
    unit/core/unit_FirmwareImage.cpp
    unit/core/unit_H264Bitstream.cpp
    unit/core/unit_AllocTracking.cpp
    unit/core/unit_HttpServerModule_apply.cpp
    unit/core/unit_ImprovFrame.cpp
    unit/core/unit_ImprovOpReassembler.cpp
    unit/core/unit_JsonUtil_parse.cpp
    unit/core/unit_JsonUtil_parseint.cpp
    unit/core/unit_JsonSink_detach.cpp
    unit/core/unit_JsonSink_number.cpp
    unit/core/unit_JsonSink_overflow.cpp
    unit/core/unit_MappingLUT.cpp
    unit/core/unit_ModuleFactory.cpp
    unit/core/unit_moonlive_fill.cpp
    unit/core/unit_moonlive_compiler.cpp
    unit/core/unit_moonlive_ir.cpp
    unit/core/unit_moonlive_spill.cpp
    unit/core/unit_moonlive_codegen_xtensa.cpp
    unit/core/unit_moonlive_codegen_riscv.cpp
    unit/core/unit_moonlive_codegen_x86_64.cpp
    unit/core/unit_moonlive_codegen_arm64.cpp
    unit/core/unit_MoonModule.cpp
    unit/core/unit_MoonModule_control_change_gate.cpp
    unit/core/unit_MoonModule_lifecycle.cpp
    unit/core/unit_MoonModule_movechild.cpp
    unit/core/unit_MoonModule_replacechild.cpp
    unit/core/unit_MoonBaseContract.cpp
    unit/core/unit_NetworkModule.cpp
    unit/core/unit_NetworkModule_ethernet.cpp
    unit/core/unit_Sort.cpp
    unit/core/unit_sanitizeHostname.cpp
    unit/core/unit_platform_clock.cpp
    unit/core/unit_platform_worker.cpp
    unit/core/unit_IpList.cpp
    unit/core/unit_Scheduler_unique_names.cpp
    unit/core/unit_MoonStatsModule.cpp
    unit/core/unit_MoonTalkModule.cpp
    unit/core/unit_InstallationId.cpp
    unit/core/unit_sha256.cpp
    unit/core/unit_MoonStatsReport.cpp
    unit/core/unit_SystemModule.cpp
    unit/core/unit_Services.cpp

    # light/
    unit/light/unit_AudioService.cpp
    unit/light/unit_AudioLevel.cpp
    unit/light/unit_AudioBands.cpp
    unit/light/unit_NetworkSendDriver_no_alloc_in_loop.cpp
    unit/light/unit_NetworkSendDriver_packet.cpp
    unit/light/unit_NdiDriver.cpp
    unit/light/unit_HlsDriver.cpp
    unit/light/unit_MpegTs.cpp
    unit/light/unit_RtpH264.cpp
    unit/light/unit_RtspSession.cpp
    unit/light/unit_PanelCardDriver.cpp
    unit/light/unit_PanelCardDriver_packet.cpp
    unit/light/unit_WledAudioSyncPacket.cpp
    unit/light/unit_BlendMap.cpp
    unit/light/unit_BeatRipples.cpp
    unit/light/unit_ColorTrailsEffect.cpp
    unit/light/unit_fluid.cpp
    unit/light/unit_draw.cpp
    unit/light/unit_GameOfLifeEffect.cpp
    unit/light/unit_DemoReelEffect.cpp
    unit/light/unit_TextEffect.cpp
    unit/light/unit_Coord3D.cpp
    unit/light/unit_effects_render.cpp
    unit/light/unit_Effects_gridsweep.cpp
    unit/core/unit_OscPacket.cpp
    unit/light/unit_Effects_golden.cpp
    unit/light/unit_SpritePopulation.cpp
    unit/light/unit_Canvas.cpp
    unit/light/unit_Splat.cpp
    unit/light/unit_Sdf.cpp
    unit/light/unit_Bar.cpp
    unit/light/unit_Scroll.cpp
    unit/light/unit_Circle.cpp
    unit/light/unit_Particles.cpp
    unit/light/unit_Shader.cpp
    unit/light/unit_PolarLut.cpp
    unit/light/unit_PolarLut_equivalence.cpp
    unit/light/unit_AuroraEffect.cpp
    unit/light/unit_NoiseEffect.cpp
    unit/light/unit_Effects_framerate.cpp
    unit/light/unit_SineEffect.cpp
    unit/light/unit_DistortionWavesEffect.cpp
    unit/light/unit_Correction.cpp
    unit/light/unit_MovingHeadEffect.cpp
    unit/light/unit_FixedPointEffect.cpp
    unit/light/unit_Drivers_container.cpp
    unit/light/unit_Drivers_livePalettes.cpp
    unit/light/unit_Drivers_rendersplit.cpp
    unit/light/unit_LightPresetsModule.cpp
    unit/light/unit_Drivers_firstOutputRgb.cpp
    unit/light/unit_HueDriver.cpp
    unit/light/unit_Palette.cpp
    unit/light/unit_WaveEffect.cpp
    unit/light/unit_FireEffect.cpp
    unit/light/unit_GridLayout.cpp
    unit/light/unit_Rings241Layout.cpp
    unit/light/unit_SingleColumnLayout.cpp
    unit/light/unit_SphereLayout.cpp
    unit/light/unit_WheelLayout.cpp
    unit/light/unit_Layer_extrude.cpp
    unit/light/unit_Layer_persistence.cpp
    unit/light/unit_Layer_sparse_mapping.cpp
    unit/light/unit_Layer_modifier_chain.cpp
    unit/light/unit_Layer_live_modifier.cpp
    unit/light/unit_Layer_phase_animation.cpp
    unit/light/unit_Layer_zero_grid.cpp
    unit/light/unit_Effects_container.cpp
    unit/light/unit_MoonLivePalette.cpp
    unit/light/unit_MoonLiveModifier.cpp
    unit/light/unit_MoonLiveDrawing.cpp
    unit/light/unit_MoonLiveParticles.cpp
    unit/light/unit_MoonLiveLayout.cpp
    unit/light/unit_MoonLiveScripts.cpp
    unit/light/unit_MoonLiveScriptResolve.cpp
    unit/light/unit_MoonLiveMotion.cpp
    unit/light/unit_Layouts_container.cpp
    unit/light/unit_Layouts_mutation.cpp
    unit/light/unit_Layouts_toggle_cycle.cpp
    unit/light/unit_MetaballsEffect.cpp
    unit/light/unit_MultiplyModifier.cpp
    unit/light/unit_CheckerboardModifier.cpp
    unit/light/unit_RandomMapModifier.cpp
    unit/light/unit_RotateModifier.cpp
    unit/light/unit_RegionModifier.cpp
    unit/light/unit_PinwheelModifier.cpp
    unit/light/unit_ParticlesEffect.cpp
    unit/light/unit_PlasmaEffect.cpp
    unit/light/unit_PreviewDriver.cpp
    unit/light/unit_RainbowEffect.cpp
    unit/light/unit_NetworkReceiveEffect.cpp
    unit/light/unit_NetworkReceiveEffect_protocols.cpp
    unit/light/unit_I80Peripheral.cpp
    unit/light/unit_MoonI80Peripheral.cpp
    unit/light/unit_Hub75Driver_pins.cpp
    unit/light/unit_Hub75Slots.cpp
    unit/light/unit_ParallelSlots.cpp
    unit/light/unit_ParlioPeripheral.cpp
    unit/light/unit_ParallelLedDriver_doublebuffer.cpp
    unit/light/unit_ParallelLedDriver_pinexpander.cpp
    unit/light/unit_ParallelLedDriver_ring.cpp
    unit/light/unit_ParallelLedDriver_swap.cpp
    unit/light/unit_RmtLedEncoder.cpp
    unit/light/unit_RmtLedDriver_lifecycle.cpp
    unit/light/unit_RmtLedDriver_pins.cpp
)

# The grid-size sweep drives every effect, including ones added after this file was
# written, so its effect list is DERIVED from src/light/effects/ rather than typed here
# (a hand-kept list would silently miss the next effect — the exact gap the sweep closes).
# Always out-of-date on purpose, same rule as build_info.h: adding an effect file must
# regenerate. The generator rewrites only on change, so an unchanged effect set is a no-op.
add_custom_target(effect_sweep_gen ALL
    COMMAND ${UV_EXECUTABLE} run python ${CMAKE_SOURCE_DIR}/moondeck/build/generate_effect_sweep.py ${CMAKE_CURRENT_BINARY_DIR}/generated/effect_sweep.h
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/effect_sweep.h
    COMMENT "Generating effect_sweep.h (every effect, for the grid-size sweep)"
    VERBATIM
)

target_include_directories(mm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mm_tests PRIVATE mm_core mm_platform)

# doctest::Approx takes a double, so every CHECK against a float literal promotes — inside
# doctest.h itself, which is vendored. -Wdouble-promotion stays -Werror for the firmware
# (the Xtensa has no FPU, so a stray promotion is a silent softfloat call in the render
# path); it just cannot also police a third-party header. Test-target only, and only the
# error, not the warning. Surfaced by the RTSan lane: clang diagnoses this where GCC does not.
if(NOT MSVC)
    target_compile_options(mm_tests PRIVATE -Wno-error=double-promotion)
endif()
add_dependencies(mm_tests effect_sweep_gen)

add_test(NAME unit_tests COMMAND mm_tests)

# Pin the desktop filesystem root into the build tree. ctest runs from the build directory, which
# is not a repo checkout, so the default would resolve to the OS per-user data directory and any
# test touching the default root would write into the developer's real settings.
set_tests_properties(unit_tests PROPERTIES
    ENVIRONMENT "MM_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/fs-root")

# Scenario runner (replays scenario JSON files in-process)
# The same shared type registry the app compiles, so a scenario can name any module the
# firmware has: src/module_types.cpp carries the reason.
add_executable(mm_scenarios scenario_runner.cpp ${CMAKE_SOURCE_DIR}/src/module_types.cpp)
target_link_libraries(mm_scenarios PRIVATE mm_core mm_platform)

# Kernel micro-bench: ns per call for the power-function kernels, on the host. A report, not a
# test (no ctest registration): the numbers go into performance.md and gate a kernel swap.
add_executable(mm_bench bench/bench_kernels.cpp)
target_link_libraries(mm_bench PRIVATE mm_core mm_platform)

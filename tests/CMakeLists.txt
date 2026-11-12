find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(T ${CMAKE_CURRENT_BINARY_DIR})
set(GEN ${T}/recompiled)

# 1) synthetic discs  2) extract test.iso  3) recompile its main.dol
add_custom_command(
  OUTPUT ${T}/test.iso ${T}/wrong.iso ${T}/junk.bin ${T}/test.dol
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_test_iso.py ${T}
  DEPENDS make_test_iso.py)
add_custom_command(
  OUTPUT ${GEN}/funcs_000.cpp ${GEN}/func_table.cpp ${GEN}/recomp_funcs.h
  COMMAND $<TARGET_FILE:ssne-recomp> ${T}/test.dol ${GEN}
  DEPENDS ssne-recomp ${T}/test.dol)

add_executable(cpu_test cpu_test_main.cpp ${GEN}/funcs_000.cpp ${GEN}/func_table.cpp)
target_include_directories(cpu_test PRIVATE ${GEN})
target_link_libraries(cpu_test PRIVATE ssne-runtime)

add_test(NAME verify_accepts_game  COMMAND gctool verify ${T}/test.iso ${CMAKE_SOURCE_DIR}/data/known_dumps.txt)
add_test(NAME verify_rejects_other COMMAND gctool verify ${T}/wrong.iso)
set_tests_properties(verify_rejects_other PROPERTIES WILL_FAIL TRUE)
add_test(NAME verify_rejects_junk  COMMAND gctool verify ${T}/junk.bin)
set_tests_properties(verify_rejects_junk PROPERTIES WILL_FAIL TRUE)
add_test(NAME extract COMMAND gctool extract ${T}/test.iso ${T}/extracted)
add_test(NAME recompiled_cpu COMMAND cpu_test ${T}/extracted)
set_tests_properties(recompiled_cpu PROPERTIES DEPENDS extract)
add_test(NAME symbols_tool COMMAND ssne-symbols ${T}/test.dol ${T}/test.sym)

foreach(test session_authority world_state world_delta_copy)
  add_executable(coop_${test}_test coop_${test}_test.cpp)
  if(test STREQUAL "world_delta_copy")
    target_sources(coop_${test}_test PRIVATE fixtures/world_state_v150_reference.cpp)
  endif()
  target_link_libraries(coop_${test}_test PRIVATE ssne-coop)
  add_test(NAME coop_${test} COMMAND coop_${test}_test)
endforeach()
if(WIN32)
  foreach(test authoritative_session network_conditions snapshot_alias bots native_frontend)
    add_executable(coop_${test}_test coop_${test}_test.cpp)
    target_link_libraries(coop_${test}_test PRIVATE ssne-coop)
    add_test(NAME coop_${test} COMMAND coop_${test}_test)
    set_tests_properties(coop_${test} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
add_executable(coop_logical_input_test coop_logical_input_test.cpp ../port/src/no_game_code.cpp)
target_link_libraries(coop_logical_input_test PRIVATE ssne-runtime)
add_test(NAME coop_logical_input COMMAND coop_logical_input_test)
add_executable(settings_127_test settings_127_test.cpp ../port/src/settings.cpp)
target_include_directories(settings_127_test PRIVATE ../port/src)
target_link_libraries(settings_127_test PRIVATE SDL2::SDL2-static)
add_test(NAME settings_127_abi_roundtrip COMMAND settings_127_test)
add_executable(controller_127_test controller_127_test.cpp ../port/src/settings.cpp)
target_include_directories(controller_127_test PRIVATE ../port/src ../runtime/include)
target_link_libraries(controller_127_test PRIVATE SDL2::SDL2-static)
add_test(NAME controller_127_virtual_device COMMAND controller_127_test)
foreach(test original_hook performance_sampling gpu_frame_drop)
  add_executable(${test}_test ${test}_test.cpp)
  target_include_directories(${test}_test PRIVATE ../runtime/include)
  target_link_libraries(${test}_test PRIVATE Threads::Threads)
  add_test(NAME ${test} COMMAND ${test}_test)
endforeach()

add_executable(native_gait_motion_test native_gait_motion_test.cpp)
target_include_directories(native_gait_motion_test PRIVATE ../runtime/include)
add_test(NAME native_gait_motion COMMAND native_gait_motion_test)
add_executable(mods_projection_test mods_test.cpp ../runtime/src/mods.cpp)
target_include_directories(mods_projection_test PRIVATE ../runtime/include ../libgc/include)
if(MSVC)
  target_compile_options(mods_projection_test PRIVATE /UNDEBUG)
else()
  target_compile_options(mods_projection_test PRIVATE -UNDEBUG)
endif()
add_test(NAME mods_projection_usa COMMAND mods_projection_test)
add_test(NAME mods_projection_pal COMMAND mods_projection_test PAL)

add_executable(native_results_countdown_test native_results_countdown_test.cpp)
target_include_directories(native_results_countdown_test PRIVATE ../runtime/include)
add_test(NAME native_results_countdown COMMAND native_results_countdown_test)

add_executable(native_animation_lifetime_test native_animation_lifetime_test.cpp)
target_include_directories(native_animation_lifetime_test PRIVATE ../runtime/include)
add_test(NAME native_animation_lifetime COMMAND native_animation_lifetime_test)

add_executable(game_view_geometry_test game_view_geometry_test.cpp)
target_include_directories(game_view_geometry_test PRIVATE ../port/src)
add_test(NAME game_view_geometry COMMAND game_view_geometry_test)

add_executable(native_menu_mouse_test native_menu_mouse_test.cpp ../port/src/native_menu_mouse.cpp)
target_include_directories(native_menu_mouse_test PRIVATE ../runtime/include ../port/src)
target_link_libraries(native_menu_mouse_test PRIVATE Threads::Threads)
add_test(NAME native_menu_mouse COMMAND native_menu_mouse_test)

add_executable(native_info_mouse_test native_info_mouse_test.cpp)
target_include_directories(native_info_mouse_test PRIVATE ../runtime/include ../port/src)
target_link_libraries(native_info_mouse_test PRIVATE Threads::Threads)
add_test(NAME native_info_mouse COMMAND native_info_mouse_test)

# Launcher patch validation uses synthetic source only; no game/runtime dependency.
add_executable(launcher_json_test launcher_json_test.cpp ../launcher/src/launcher_json.cpp)
add_test(NAME launcher_json COMMAND launcher_json_test)
add_executable(translation_patch_test translation_patch_test.cpp ../launcher/src/launcher_json.cpp ../launcher/src/translation_patch.cpp ../libgc/src/sha256.cpp)
target_include_directories(translation_patch_test PRIVATE ../libgc/include)
add_test(NAME translation_patch COMMAND translation_patch_test)

add_executable(launcher_update_test launcher_update_test.cpp ../launcher/src/updater.cpp ../launcher/src/launcher_json.cpp ../launcher/src/runtime_kit.cpp ../libgc/src/sha256.cpp)
target_include_directories(launcher_update_test PRIVATE ../libgc/include)
target_compile_definitions(launcher_update_test PRIVATE SSNE_VERSION="0.0.0" SSNE_UPDATE_REPOSITORY="${SSNE_UPDATE_REPOSITORY}")
target_link_libraries(launcher_update_test PRIVATE Threads::Threads)
add_test(NAME launcher_update COMMAND launcher_update_test)

add_executable(launcher_backend_test launcher_backend_test.cpp ../launcher/src/runtime_kit.cpp
  ../launcher/src/launcher_json.cpp ../launcher/src/translation_patch.cpp ../libgc/src/sha256.cpp)
target_include_directories(launcher_backend_test PRIVATE ../libgc/include)
target_link_libraries(launcher_backend_test PRIVATE Threads::Threads)
add_test(NAME launcher_backend COMMAND launcher_backend_test)

add_executable(crosshair_image_test crosshair_image_test.cpp ../port/src/crosshair_image.cpp)
target_include_directories(crosshair_image_test PRIVATE ../port/src)
target_link_libraries(crosshair_image_test PRIVATE gc)
add_test(NAME crosshair_image COMMAND crosshair_image_test)
add_executable(crosshair_settings_test crosshair_settings_test.cpp ../port/src/settings.cpp)
target_include_directories(crosshair_settings_test PRIVATE ../port/src)
target_link_libraries(crosshair_settings_test PRIVATE SDL2::SDL2-static)
add_test(NAME crosshair_settings COMMAND crosshair_settings_test)

add_executable(native_crosshair_test native_crosshair_test.cpp)
target_include_directories(native_crosshair_test PRIVATE ../runtime/include)
add_test(NAME native_crosshair COMMAND native_crosshair_test)

add_executable(gx_crosshair_test gx_crosshair_test.cpp ../runtime/src/gx/gx_crosshair.cpp)
target_include_directories(gx_crosshair_test PRIVATE ../runtime/include)
target_link_libraries(gx_crosshair_test PRIVATE Threads::Threads)
add_test(NAME gx_crosshair COMMAND gx_crosshair_test)

add_executable(native_vehicle_input_test native_vehicle_input_test.cpp)
target_include_directories(native_vehicle_input_test PRIVATE ../runtime/src)
target_link_libraries(native_vehicle_input_test PRIVATE Threads::Threads)
add_test(NAME native_vehicle_input COMMAND native_vehicle_input_test)

add_executable(ammo_rules_test ammo_rules_test.cpp ../runtime/src/ammo_rules.cpp)
target_include_directories(ammo_rules_test PRIVATE ../runtime/include)
add_test(NAME ammo_rules COMMAND ammo_rules_test)

add_executable(settings_menu_language_test settings_menu_language_test.cpp)
target_include_directories(settings_menu_language_test PRIVATE ../port/src)
target_link_libraries(settings_menu_language_test PRIVATE imgui)
add_test(NAME settings_menu_language COMMAND settings_menu_language_test)
